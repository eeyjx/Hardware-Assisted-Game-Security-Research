import { useEffect, useMemo, useRef, useState, type CSSProperties } from 'react';
import type { RadarSettings } from '../hooks/useRadarSettings';
import { formatCountdown, getBombTiming, estimateHostNowMs } from '../lib/bomb';
import {
  cssHeadingFromGameYaw,
  projectWorldPoint,
  resolveMapImage,
  selectStableMapLevel,
  selectReferenceZ,
  unwrapHeading,
} from '../lib/coordinates';
import { bombStateLabel } from '../lib/format';
import type {
  BombSnapshot,
  BombState,
  MapDefinition,
  MapLevelDefinition,
  PlayerSnapshot,
  Team,
  Vector3,
} from '../types/protocol';

interface RadarMapProps {
  mapId: string;
  map: MapDefinition | undefined;
  players: PlayerSnapshot[];
  localPlayerId?: string | null;
  observedPlayerId?: string | null;
  localTeam?: Team | null;
  bomb: BombSnapshot;
  capturedAtMs: number | null;
  receivedAtPerformanceMs: number | null;
  performanceNowMs: number;
  settings: RadarSettings;
  stale: boolean;
  staleMessage?: string;
  manifestError: string | null;
  manifestLoading: boolean;
  onRetryMap: () => void;
}

const competitivePalette = ['#9aa4b2', '#5ab4ff', '#b88cff', '#72d69d', '#ffd166', '#ff8c69'];
const visibleBombStates = new Set<BombState>(['carried', 'dropped', 'planted']);

/** 优先使用竞技颜色；缺失或越界时退回阵营颜色。 */
function playerAccent(player: PlayerSnapshot): string {
  if (
    Number.isInteger(player.competitiveColor) &&
    player.competitiveColor !== undefined &&
    player.competitiveColor >= 0 &&
    player.competitiveColor < competitivePalette.length
  ) {
    return competitivePalette[player.competitiveColor];
  }
  return player.team === 'CT' ? '#56c7f2' : player.team === 'T' ? '#f1b95b' : '#a1a8af';
}

/** 标记玩家相对当前底图楼层的位置；maxZ 边界属于上层。 */
function getFloorOffset(
  position: Vector3,
  level: MapLevelDefinition | undefined,
): 'lower' | 'upper' | null {
  if (!level) return null;
  if (position.z < level.minZ) return 'lower';
  if (position.z >= level.maxZ) return 'upper';
  return null;
}

/** 炸弹坐标依次取显式位置、载弹玩家 ID，最后兼容 hasBomb 标记。 */
function resolveBombPosition(
  bomb: BombSnapshot,
  players: PlayerSnapshot[],
): Vector3 | null | undefined {
  if (bomb.position) return bomb.position;
  const carrierId = bomb.carrierPlayerId ?? bomb.carrierId;
  if (carrierId) return players.find((player) => player.id === carrierId)?.position;
  return players.find((player) => player.hasBomb)?.position;
}

/** 地图缺失时的标题与说明；保持错误、加载和等待状态的原有优先级。 */
function getMapPlaceholderCopy(
  mapId: string,
  loading: boolean,
  error: string | null,
): { heading: string; detail: string } {
  const heading = loading
    ? '正在加载固定地图资源'
    : mapId
      ? `缺少 ${mapId} 的地图定义`
      : '等待游戏地图';
  const detail = error
    ? `地图清单加载失败：${error}`
    : loading
      ? '正在读取地图清单…'
      : mapId
        ? '当前地图尚未收录到地图清单。'
        : '收到游戏快照后会自动显示。';
  return { heading, detail };
}

/** 固定北向 Radar：绘制地图、玩家、炸弹、楼层和暂停状态。 */
export function RadarMap({
  mapId,
  map,
  players,
  localPlayerId,
  observedPlayerId,
  localTeam,
  bomb,
  capturedAtMs,
  receivedAtPerformanceMs,
  performanceNowMs,
  settings,
  stale,
  staleMessage,
  manifestError,
  manifestLoading,
  onRetryMap,
}: RadarMapProps) {
  // revision 仅在重试时递增，用查询参数绕过浏览器中的失败图片缓存。
  const [imageFailed, setImageFailed] = useState(false);
  const [imageRevision, setImageRevision] = useState(0);

  // 航向历史用于跨越 0°/360° 时保持旋转动画走最短路径。
  const playerHeadings = useRef({
    mapId,
    values: new Map<string, number>(),
  });
  // 保存上次稳定楼层，为楼层边界提供迟滞，防止底图快速闪烁。
  const selectedFloor = useRef<{ mapId: string; levelId?: string }>({ mapId });
  const referenceZ = selectReferenceZ(
    players,
    localPlayerId,
    observedPlayerId,
    localTeam,
  );
  const floorSelection = map && referenceZ !== undefined
    ? selectStableMapLevel(
        map.levels,
        referenceZ,
        selectedFloor.current.mapId === mapId ? selectedFloor.current.levelId : undefined,
      )
    : { level: undefined, confidence: 0, retained: false };
  const level = floorSelection.level;
  const imageUrl = level?.image ?? (map ? resolveMapImage(map, referenceZ) : '');
  const imageSrc = imageRevision === 0
    ? imageUrl
    : `${imageUrl}${imageUrl.includes('?') ? '&' : '?'}radar_retry=${imageRevision}`;
  const headingFrame = useMemo(() => {
    const previous = playerHeadings.current.mapId === mapId
      ? playerHeadings.current.values
      : undefined;
    const next = new Map<string, number>();
    for (const player of players) {
      if (!player.alive || player.yaw === null) continue;
      next.set(
        player.id,
        unwrapHeading(
          previous?.get(player.id),
          cssHeadingFromGameYaw(player.yaw),
        ),
      );
    }
    return next;
  }, [mapId, players]);

  useEffect(() => {
    setImageFailed(false);
    setImageRevision(0);
  }, [imageUrl]);

  useEffect(() => {
    // 只提交已经完成渲染的航向；并发渲染被中断时不能污染下一帧历史。
    // mapId 同步写入，确保切换地图后立即从新的包裹角度开始。
    playerHeadings.current = { mapId, values: headingFrame };
  }, [headingFrame, mapId]);

  useEffect(() => {
    selectedFloor.current = { mapId, levelId: level?.id };
  }, [level?.id, mapId]);

  useEffect(() => {
    // 图片失败后，在重新联网、恢复页面或手动重试时自动追加 cache-buster。
    if (!imageFailed) return undefined;
    const resume = () => {
      if (document.visibilityState === 'hidden' || navigator.onLine === false) return;
      setImageFailed(false);
      setImageRevision((revision) => revision + 1);
    };
    window.addEventListener('online', resume);
    window.addEventListener('pageshow', resume);
    document.addEventListener('visibilitychange', resume);
    return () => {
      window.removeEventListener('online', resume);
      window.removeEventListener('pageshow', resume);
      document.removeEventListener('visibilitychange', resume);
    };
  }, [imageFailed]);

  const retryMapImage = () => {
    setImageFailed(false);
    setImageRevision((revision) => revision + 1);
  };

  const bombPosition = useMemo(
    () => resolveBombPosition(bomb, players),
    [bomb.carrierId, bomb.carrierPlayerId, bomb.position, players],
  );

  // 将主机 wall-clock 快照时间与浏览器 monotonic clock 对齐，避免系统时钟跳变。
  const hostNowMs =
    capturedAtMs !== null && receivedAtPerformanceMs !== null
      ? estimateHostNowMs(
          { capturedAtMs, receivedAtPerformanceMs },
          performanceNowMs,
        )
      : 0;
  const elapsedSinceSnapshotMs =
    receivedAtPerformanceMs === null
      ? 0
      : Math.max(0, performanceNowMs - receivedAtPerformanceMs);
  const timing = getBombTiming(bomb, hostNowMs, elapsedSinceSnapshotMs);
  const visibleBomb = visibleBombStates.has(bomb.state);
  const bombPoint = map && visibleBomb && bombPosition
    ? projectWorldPoint(bombPosition, map)
    : null;
  const bombStyle = bombPoint?.inBounds
    ? {
        left: `${bombPoint.x * 100}%`,
        top: `${bombPoint.y * 100}%`,
        '--bomb-size': `${settings.bombSize}px`,
      } as CSSProperties
    : null;
  const placeholder = getMapPlaceholderCopy(mapId, manifestLoading, manifestError);

  return (
    <section className="radar-shell" aria-label="固定北向全地图 Radar">
      <div className="radar-topbar">
        <div>
          <span className="north-indicator"><i />N</span>
          <strong>{map?.name ?? (mapId || 'NO MAP')}</strong>
          {level && (floorSelection.confidence < 0.5 ? (
            <small
              className="floor-confidence-low"
              title={floorSelection.retained ? '楼层边界缓冲中' : '楼层判断置信度较低'}
            >
              {level.id} · 切换中
            </small>
          ) : <small>{level.id}</small>)}
        </div>
        <span className={`bomb-status bomb-${bomb.state}`}>{bombStateLabel(bomb.state)}</span>
      </div>

      <div className="radar-frame">
        <div className="radar-surface">
          {/* 静态地图底图；加载失败时由下方占位层接管。 */}
          {map && !imageFailed && (
            <img
              className="radar-image"
              src={imageSrc}
              alt=""
              aria-hidden="true"
              draggable={false}
              onError={() => setImageFailed(true)}
            />
          )}

          {/* 玩家标记保留既有 class 顺序与 mapId key，以维持动画和测试契约。 */}
          {map &&
            players.map((player) => {
              if (!player.position) return null;
              const point = projectWorldPoint(player.position, map);
              if (!point.inBounds || player.team === 'NONE') return null;
              const floorOffset = getFloorOffset(player.position, level);
              const heading = headingFrame.get(player.id);
              const style = {
                left: `${point.x * 100}%`,
                top: `${point.y * 100}%`,
                '--player-size': `${settings.playerSize}px`,
                '--player-accent': playerAccent(player),
                '--heading': `${heading ?? 0}deg`,
              } as CSSProperties;
              // key 包含 mapId：换图时强制重建节点，禁止 CSS 从旧地图角度插值。
              const markerKey = `${mapId}:${player.id}`;
              return (
                <div
                  className={`map-player team-${player.team.toLowerCase()} ${
                    player.alive ? 'is-alive' : 'is-dead'
                  } ${player.dormant ? 'is-dormant' : ''} ${
                    player.id === localPlayerId ? 'is-local' : ''
                  }`}
                  style={style}
                  key={markerKey}
                  title={`${player.name || 'ANONYMOUS'} · ${player.health} HP`}
                >
                  {player.alive ? (
                    <i className={`direction-arrow ${heading === undefined ? 'has-no-heading' : ''}`} />
                  ) : <i className="death-mark">×</i>}
                  {floorOffset && (
                    <i className={`floor-mark is-${floorOffset}`}>
                      {floorOffset === 'upper' ? '↑' : '↓'}
                    </i>
                  )}
                  {player.hasBomb && <i className="carrier-mark">C4</i>}
                  {settings.showNames && <span>{player.name || 'ANONYMOUS'}</span>}
                </div>
              );
            })}

          {/* 独立 C4 标记只显示携带、掉落和已安放状态。 */}
          {bombStyle && (
            <div
              className={`map-bomb bomb-${bomb.state}`}
              style={bombStyle}
              title={bombStateLabel(bomb.state)}
            >
              C4
            </div>
          )}

          {/* 没有地图定义时，显示加载、错误或等待快照状态。 */}
          {!map && (
            <div className="map-placeholder">
              <span aria-hidden="true">⌖</span>
              <strong>{placeholder.heading}</strong>
              <p>{placeholder.detail}</p>
              {manifestError && (
                <button type="button" onClick={onRetryMap}>重新加载地图清单</button>
              )}
            </div>
          )}

          {/* 图片失败与数据暂停是覆盖层，不会清空最后一帧玩家数据。 */}
          {map && imageFailed && (
            <div className="map-placeholder image-missing">
              <span aria-hidden="true">▧</span>
              <strong>地图图像不可用</strong>
              <p>固定地图静态资源加载失败。</p>
              <button type="button" onClick={retryMapImage}>重新加载地图图像</button>
            </div>
          )}

          {stale && (
            <div className="stale-overlay" role="status" aria-live="polite">
              <strong>数据已暂停</strong>
              <span>{staleMessage ?? '正在等待新的游戏快照'}</span>
            </div>
          )}
        </div>

        {/* 已安放时始终显示倒计时，即使炸弹坐标暂时不可用。 */}
        {bomb.state === 'planted' && (
          <div className="bomb-timer" role="timer" aria-live="off" aria-label="炸弹倒计时">
            <span className="site-badge">
              {bomb.site && bomb.site !== 'unknown' ? bomb.site.toUpperCase() : '?'}
            </span>
            <div>
              <small>爆炸倒计时</small>
              <strong>{formatCountdown(timing.explosionRemainingMs)}s</strong>
            </div>
            {bomb.beingDefused && (
              <div className={timing.canCompleteDefuse ? 'defuse-safe' : 'defuse-danger'}>
                <small>拆除倒计时</small>
                <strong>{formatCountdown(timing.defuseRemainingMs)}s</strong>
                <span>{timing.canCompleteDefuse ? '可完成' : '来不及'}</span>
              </div>
            )}
          </div>
        )}
      </div>
    </section>
  );
}

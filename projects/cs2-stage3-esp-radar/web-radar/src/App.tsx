import { useEffect, useMemo, useState } from 'react';
import { ConnectionNotice } from './components/ConnectionNotice';
import { PlayerPanel } from './components/PlayerPanel';
import { RadarFooter } from './components/RadarFooter';
import { RadarMap } from './components/RadarMap';
import { RelayAccessGate } from './components/RelayAccessGate';
import { SettingsPanel } from './components/SettingsPanel';
import { StatusHeader } from './components/StatusHeader';
import { useAnimationClock } from './hooks/useAnimationClock';
import { useMapManifest } from './hooks/useMapManifest';
import { useRadarSettings } from './hooks/useRadarSettings';
import { useRadarStream } from './hooks/useRadarStream';
import { useSnapshotReplay } from './hooks/useSnapshotReplay';
import { useRelaySession } from './hooks/useRelaySession';
import {
  resolveRadarDeployment,
  sanitizeRelayUrl,
  type RadarDeploymentMode,
} from './lib/deployment';
import type { BombSnapshot } from './types/protocol';

interface RadarWorkspaceProps {
  mode: RadarDeploymentMode;
  room: string | null;
  onSessionRejected: () => void;
  onLogout?: () => void;
  logoutPending: boolean;
}

// 无快照时复用同一个只读占位对象，避免每次渲染创建临时值。
const EMPTY_BOMB: BombSnapshot = { state: 'unknown' };

/**
 * Radar 主工作区：组合数据流、回放、地图清单和用户显示设置。
 * 该组件只做状态编排，具体显示细节交给下方的独立组件处理。
 */
function RadarWorkspace({
  mode,
  room,
  onSessionRejected,
  onLogout,
  logoutPending,
}: RadarWorkspaceProps) {
  const stream = useRadarStream({ mode, onSessionRejected });
  const replay = useSnapshotReplay();
  const maps = useMapManifest();
  const { settings, setSettings } = useRadarSettings();
  const [settingsOpen, setSettingsOpen] = useState(false);

  // 回放帧优先于实时帧，但不会修改实时连接本身的状态。
  const frame = replay.frame ?? stream.frame;
  const snapshot = frame?.snapshot;
  const isReplay = replay.active;
  const mapId = snapshot?.map.id ?? '';
  const map = maps.manifest?.maps.find((entry) => entry.id === mapId);
  const dataStale = isReplay ? false : stream.stale;
  const displayStatus = isReplay ? 'connected' : stream.status;
  const displayError = isReplay ? null : stream.error;
  const performanceNowMs = useAnimationClock(snapshot?.bomb.state === 'planted' && !dataStale);
  const players = snapshot?.players ?? [];
  const playerPanelProps = {
    players,
    localPlayerId: snapshot?.localPlayerId,
    showEquipment: settings.showEquipment,
  };

  return (
    <div className="app-shell">
      <StatusHeader
        status={displayStatus}
        stale={dataStale}
        mapName={map?.name ?? snapshot?.map.displayName ?? mapId}
        sequence={snapshot?.seq ?? null}
        error={displayError}
        settingsOpen={settingsOpen}
        deploymentLabel={mode === 'relay' ? `RELAY · ${room ?? ''}` : 'LOCAL'}
        onToggleSettings={() => setSettingsOpen((value) => !value)}
        onLogout={onLogout}
        logoutPending={logoutPending}
      />

      {/* 回放仅替换显示帧；连接提示仍如原实现一样跟踪后台实时流。 */}
      <ConnectionNotice
        status={displayStatus}
        stale={stream.stale}
        error={displayError}
        retryInMs={stream.retryInMs}
        lastReceivedAtMs={frame?.receivedAtWallMs ?? null}
        hasFrame={frame !== null}
        onRetry={stream.retry}
      />

      {settingsOpen && (
        <SettingsPanel
          settings={settings}
          onChange={setSettings}
          onClose={() => setSettingsOpen(false)}
          replay={replay}
        />
      )}

      <main className="radar-layout">
        <PlayerPanel team="CT" {...playerPanelProps} />
        <RadarMap
          mapId={mapId}
          map={map}
          players={players}
          localPlayerId={snapshot?.localPlayerId}
          observedPlayerId={snapshot?.observedPlayerId}
          localTeam={snapshot?.localTeam}
          bomb={snapshot?.bomb ?? EMPTY_BOMB}
          capturedAtMs={snapshot?.capturedAtMs ?? null}
          receivedAtPerformanceMs={frame?.receivedAtPerformanceMs ?? null}
          performanceNowMs={performanceNowMs}
          settings={settings}
          stale={dataStale}
          staleMessage={stream.status === 'offline' ? '设备离线，等待网络恢复' : undefined}
          manifestError={maps.error}
          manifestLoading={maps.loading}
          onRetryMap={maps.retry}
        />
        <PlayerPanel team="T" {...playerPanelProps} />
      </main>

      <RadarFooter
        replayActive={isReplay}
        mode={mode}
        playerCount={players.length}
        updateRateHz={stream.quality.updateRateHz}
        snapshotAgeMs={stream.quality.snapshotAgeMs}
        skippedFrames={stream.quality.skippedFrames}
      />
    </div>
  );
}

export default function App() {
  // 部署模式只由初始 URL 决定，页面运行期间无需重复解析。
  const mode = useMemo(() => resolveRadarDeployment(window.location), []);
  const relay = useRelaySession(mode);

  useEffect(() => {
    // Relay 邀请凭据只使用一次，随后立即从地址栏中移除。
    if (mode !== 'relay') return;
    const sanitized = sanitizeRelayUrl(window.location);
    if (sanitized !== null) window.history.replaceState(window.history.state, '', sanitized);
  }, [mode]);

  if (mode === 'relay' && relay.access.status !== 'authenticated') {
    // 公网 Relay 在认证完成前绝不挂载 Radar 数据工作区。
    return (
      <RelayAccessGate
        access={relay.access}
        submitting={relay.submitting}
        actionError={relay.actionError}
        onLogin={relay.login}
        onRetry={relay.retry}
      />
    );
  }

  const room = relay.access.status === 'authenticated' ? relay.access.session.room : null;
  return (
    <RadarWorkspace
      mode={mode}
      room={room}
      onSessionRejected={relay.invalidate}
      onLogout={mode === 'relay' ? () => void relay.logout() : undefined}
      logoutPending={relay.submitting}
    />
  );
}

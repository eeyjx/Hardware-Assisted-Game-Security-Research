import type { RadarDeploymentMode } from '../lib/deployment';

interface RadarFooterProps {
  replayActive: boolean;
  mode: RadarDeploymentMode;
  playerCount: number;
  updateRateHz: number;
  snapshotAgeMs: number;
  skippedFrames: number;
}

/**
 * 页面底部只负责展示图例与实时质量指标。
 * 将可选指标整理成数组，可避免在 JSX 中重复多层条件判断。
 */
export function RadarFooter({
  replayActive,
  mode,
  playerCount,
  updateRateHz,
  snapshotAgeMs,
  skippedFrames,
}: RadarFooterProps) {
  const sourceLabel = replayActive ? '本地回放' : mode === 'relay' ? '安全 Relay' : '内嵌服务';
  const liveMetrics = replayActive
    ? []
    : [
        updateRateHz > 0 ? `${updateRateHz.toFixed(1)} Hz` : '',
        snapshotAgeMs > 0 ? `数据龄 ${Math.round(snapshotAgeMs)} ms` : '',
        skippedFrames > 0 ? `跳过 ${skippedFrames} 帧` : '',
      ].filter(Boolean);

  return (
    <footer className="app-footer">
      <span><i className="legend-dot ct" /> CT</span>
      <span><i className="legend-dot t" /> T</span>
      <span><i className="legend-cross">×</i> 阵亡</span>
      <span className="footer-spacer" />
      <span>{sourceLabel}</span>
      <span>协议 v1</span>
      {liveMetrics.map((metric) => <span key={metric}>{metric}</span>)}
      <span>{playerCount} 名玩家</span>
    </footer>
  );
}

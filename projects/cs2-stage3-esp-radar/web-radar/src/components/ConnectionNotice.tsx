import type { StreamStatus } from '../lib/stream';

interface ConnectionNoticeProps {
  status: StreamStatus;
  stale: boolean;
  error: string | null;
  retryInMs: number | null;
  lastReceivedAtMs: number | null;
  hasFrame: boolean;
  onRetry: () => void;
}

interface NoticeCopy {
  heading: string;
  detail: string;
}

// 时间格式器只需创建一次；每次渲染仅格式化最新快照时间。
const snapshotTimeFormatter = new Intl.DateTimeFormat(undefined, {
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
});

function formatLastSnapshot(timestamp: number | null): string | null {
  return timestamp === null ? null : snapshotTimeFormatter.format(new Date(timestamp));
}

/** 将连接状态转换成用户可读文案，不在显示组件中混入状态判断细节。 */
function getNoticeCopy(
  status: StreamStatus,
  stale: boolean,
  error: string | null,
  retryInMs: number | null,
  hasFrame: boolean,
): NoticeCopy {
  switch (status) {
    case 'offline':
      return { heading: '设备当前离线', detail: '网络恢复后会自动重连，也可以手动重试。' };
    case 'connecting':
      return { heading: '正在连接 Radar', detail: '正在建立安全的实时数据连接…' };
    case 'reconnecting':
      return {
        heading: '实时连接已中断',
        detail: retryInMs === null
          ? (error ?? '正在重新连接…')
          : `${error ?? '连接暂时不可用'}；约 ${Math.max(1, Math.ceil(retryInMs / 1_000))} 秒后自动重试。`,
      };
    case 'connected':
      if (stale && hasFrame) {
        return {
          heading: '游戏数据已暂停',
          detail: '超过 3 秒未收到新快照；可能是 Producer 断线、游戏暂停或页面刚从后台恢复。',
        };
      }
      if (stale) {
        return { heading: '已连接，正在等待首个快照', detail: 'Relay 已连接，但 Producer 尚未发布游戏数据。' };
      }
      break;
    case 'disconnected':
      return { heading: 'Radar 会话已断开', detail: error ?? '正在恢复 Radar 数据流' };
  }

  return { heading: '连接状态异常', detail: error ?? '正在恢复 Radar 数据流' };
}

/** 仅在实时流不健康时出现的非阻塞连接提示。 */
export function ConnectionNotice({
  status,
  stale,
  error,
  retryInMs,
  lastReceivedAtMs,
  hasFrame,
  onRetry,
}: ConnectionNoticeProps) {
  // 健康连接不占用任何页面空间。
  if (status === 'connected' && !stale && !error) return null;

  const { heading, detail } = getNoticeCopy(status, stale, error, retryInMs, hasFrame);
  const lastSnapshot = formatLastSnapshot(lastReceivedAtMs);
  return (
    <aside className="connection-notice" role="status" aria-live="polite" aria-atomic="true">
      <span className="connection-notice-mark" aria-hidden="true">!</span>
      <div>
        <strong>{heading}</strong>
        <span>{detail}</span>
        {lastSnapshot && <small>最近快照：{lastSnapshot}</small>}
      </div>
      <button type="button" onClick={onRetry}>立即重试</button>
    </aside>
  );
}

import type { ChangeEvent } from 'react';
import type { RadarSettings } from '../hooks/useRadarSettings';
import type { SnapshotReplayState } from '../hooks/useSnapshotReplay';

interface SettingsPanelProps {
  settings: RadarSettings;
  onChange: (settings: RadarSettings) => void;
  onClose: () => void;
  replay: SnapshotReplayState;
}

type RangeSettingKey = 'playerSize' | 'bombSize';
type ToggleSettingKey = 'showNames' | 'showEquipment';

// 设置项的文案和边界集中维护，避免为每个控件复制一整段 JSX。
const RANGE_SETTINGS: ReadonlyArray<{
  key: RangeSettingKey;
  label: string;
  min: number;
  max: number;
}> = [
  { key: 'playerSize', label: '玩家图标', min: 12, max: 34 },
  { key: 'bombSize', label: 'C4 图标', min: 18, max: 42 },
];

const TOGGLE_SETTINGS: ReadonlyArray<{
  key: ToggleSettingKey;
  label: string;
}> = [
  { key: 'showNames', label: '地图显示姓名' },
  { key: 'showEquipment', label: '面板显示装备' },
];

/** 显示设置与本地快照回放的统一控制面板。 */
export function SettingsPanel({ settings, onChange, onClose, replay }: SettingsPanelProps) {
  // 始终创建新对象，确保 React 能可靠识别设置变化。
  const update = <K extends keyof RadarSettings>(key: K, value: RadarSettings[K]) => {
    onChange({ ...settings, [key]: value });
  };

  const loadReplayFile = (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (file) void replay.load(file);

    // 清空 input，允许用户再次选择同一个回放文件。
    event.target.value = '';
  };

  return (
    <aside className="settings-panel" aria-label="Radar 设置">
      <div className="settings-heading">
        <div>
          <strong>显示设置</strong>
          <span>仅保存在当前浏览器</span>
        </div>
        <button type="button" onClick={onClose} aria-label="关闭设置">×</button>
      </div>

      {RANGE_SETTINGS.map(({ key, label, min, max }) => (
        <label className="range-setting" key={key}>
          <span>{label}<output>{settings[key]}px</output></span>
          <input
            type="range"
            min={min}
            max={max}
            value={settings[key]}
            onChange={(event) => update(key, Number(event.target.value))}
          />
        </label>
      ))}

      {TOGGLE_SETTINGS.map(({ key, label }) => (
        <label className="toggle-setting" key={key}>
          <span>{label}</span>
          <input
            type="checkbox"
            checked={settings[key]}
            onChange={(event) => update(key, event.target.checked)}
          />
          <i aria-hidden="true" />
        </label>
      ))}

      <div className="replay-setting">
        <strong>快照回放</strong>
        <label className="replay-file-button">
          载入 NDJSON
          <input
            type="file"
            accept=".ndjson,application/x-ndjson,application/json"
            onChange={loadReplayFile}
          />
        </label>
        {replay.active && (
          <>
            <span>{replay.fileName} · {replay.current}/{replay.total}</span>
            {/* 界面显示从 1 开始；seek API 使用从 0 开始的数组索引。 */}
            <input
              type="range"
              min="1"
              max={replay.total}
              value={replay.current}
              onChange={(event) => replay.seek(Number(event.target.value) - 1)}
            />
            <div className="replay-actions">
              <button type="button" onClick={replay.togglePlaying}>
                {replay.playing ? '暂停' : '播放'}
              </button>
              <button type="button" onClick={replay.stop}>停止回放</button>
            </div>
          </>
        )}
        {replay.error && <span className="settings-error">{replay.error}</span>}
      </div>
    </aside>
  );
}

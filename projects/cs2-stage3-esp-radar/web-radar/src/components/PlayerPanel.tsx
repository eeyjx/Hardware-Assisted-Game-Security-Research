import { compactWeaponName, teamLabel } from '../lib/format';
import type { PlayerSnapshot, Team, WeaponSnapshot } from '../types/protocol';

interface PlayerPanelProps {
  team: Extract<Team, 'T' | 'CT'>;
  players: PlayerSnapshot[];
  localPlayerId?: string | null;
  showEquipment: boolean;
}

interface PlayerCardProps {
  player: PlayerSnapshot;
  isLocal: boolean;
  showEquipment: boolean;
}

/**
 * 同时兼容当前武器对象、定义编号与名称匹配。
 * 不能使用对象引用比较，因为快照解析后会生成新的对象实例。
 */
function isActiveWeapon(weapon: WeaponSnapshot, active: WeaponSnapshot | undefined): boolean {
  return Boolean(
    weapon.active ||
    (active && (
      (weapon.definitionIndex !== undefined && weapon.definitionIndex === active.definitionIndex) ||
      weapon.name === active.name
    )),
  );
}

/** 单个玩家卡片：显示身份、经济、生命值和可选装备。 */
function PlayerCard({
  player,
  isLocal,
  showEquipment,
}: PlayerCardProps) {
  // inventory 是当前字段，weapons 是早期 v1 发布器的兼容别名。
  const weapons = player.inventory ?? player.weapons ?? [];
  const active = player.activeWeapon ?? weapons.find((weapon) => weapon.active);
  const displayName = player.name || 'ANONYMOUS';
  const visibleHealth = Math.max(0, player.health);

  return (
    <article className={`player-card ${player.alive ? '' : 'is-dead'} ${isLocal ? 'is-local' : ''}`}>
      <div className="player-card-topline">
        {player.steamId ? (
          <a
            className="player-name"
            href={`https://steamcommunity.com/profiles/${encodeURIComponent(player.steamId)}`}
            target="_blank"
            rel="noopener noreferrer"
            title="打开 Steam 社区资料"
          >
            {isLocal && <i className="local-dot" aria-label="本地玩家" />}
            {displayName}
          </a>
        ) : (
          <span className="player-name" title={player.name ?? 'ANONYMOUS'}>
            {isLocal && <i className="local-dot" aria-label="本地玩家" />}
            {displayName}
          </span>
        )}
        <span className="player-money">${Math.max(0, player.money).toLocaleString('en-US')}</span>
      </div>

      <div className="vitals-row">
        <span className="health-stat">{visibleHealth}</span>
        <span className="health-track" aria-label={`生命值 ${player.health}`}>
          <i style={{ width: `${Math.min(100, visibleHealth)}%` }} />
        </span>
        <span className="armor-stat">◇ {Math.max(0, player.armor)}</span>
      </div>

      <div className="kit-row">
        {player.hasHelmet && <span title="头盔">H</span>}
        {player.hasDefuser && <span title="拆弹器">K</span>}
        {player.hasBomb && <span className="bomb-chip" title="携带 C4">C4</span>}
        {!player.alive && <span className="dead-chip">阵亡</span>}
        {active && <strong title="当前武器">{compactWeaponName(active)}</strong>}
      </div>

      {showEquipment && weapons.length > 0 && (
        <div className="equipment-row" aria-label="装备">
          {weapons.map((weapon, index) => (
            <span
              className={isActiveWeapon(weapon, active) ? 'is-active' : ''}
              key={`${weapon.name}-${index}`}
            >
              {compactWeaponName(weapon)}
            </span>
          ))}
        </div>
      )}
    </article>
  );
}

/** 按阵营过滤并排序玩家；过滤后的新数组可安全排序，不会修改原快照。 */
export function PlayerPanel({
  team,
  players,
  localPlayerId,
  showEquipment,
}: PlayerPanelProps) {
  const teamPlayers = players
    .filter((player) => player.team === team)
    .sort(
      (a, b) =>
        Number(b.alive) - Number(a.alive) ||
        (a.name ?? '').localeCompare(b.name ?? ''),
    );
  const alive = teamPlayers.filter((player) => player.alive).length;

  return (
    <section className={`team-panel team-${team.toLowerCase()}`} aria-label={teamLabel(team)}>
      <header>
        <div>
          <span>{team}</span>
          <strong>{teamLabel(team)}</strong>
        </div>
        <output>{alive}/{teamPlayers.length}</output>
      </header>
      <div className="player-list">
        {teamPlayers.length === 0 ? (
          <p className="panel-empty">等待玩家数据</p>
        ) : (
          teamPlayers.map((player) => (
            <PlayerCard
              key={player.id}
              player={player}
              isLocal={player.id === localPlayerId}
              showEquipment={showEquipment}
            />
          ))
        )}
      </div>
    </section>
  );
}

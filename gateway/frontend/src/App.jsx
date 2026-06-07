import React, { useState, useEffect } from 'react';
import './App.css';

const POLL_MS = 3000;

const FIELD_LABELS = {
  orders_processed: 'Orders',
  trades_executed: 'Trades',
  order_tps: 'Order TPS',
  trade_tps: 'Trade TPS',
  p50: 'p50',
  p90: 'p90',
  p99: 'p99',
  final_score: 'Total',
  benchmark_score: 'Benchmark',
  throughput_score: 'Throughput',
  p99_penalty: 'p99 penalty',
  latency_ratio: 'Latency ratio',
};

const PHASE1_KEYS = [
  'orders_processed',
  'trades_executed',
  'order_tps',
  'trade_tps',
];

const PHASE2_KEYS = [
  'orders_processed',
  'trades_executed',
  'order_tps',
  'trade_tps',
  'p50',
  'p90',
  'p99',
];

const SCORE_KEYS = [
  'final_score',
  'benchmark_score',
  'throughput_score',
  'p99_penalty',
  'latency_ratio',
];

function shortJobId(id) {
  if (!id) return '—';
  if (id.length <= 16) return id;
  return `${id.slice(0, 10)}…${id.slice(-6)}`;
}

function prettyTime(ms) {
  return ms
    ? new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    : '—';
}

function formatDuration(ms) {
  if (ms == null || ms < 0) return '—';
  const totalSec = Math.floor(ms / 1000);
  if (totalSec < 60) return `${totalSec}s`;

  const min = Math.floor(totalSec / 60);
  const sec = totalSec % 60;
  if (min < 60) return sec ? `${min}m ${sec}s` : `${min}m`;

  const hr = Math.floor(min / 60);
  const remMin = min % 60;
  return remMin ? `${hr}h ${remMin}m` : `${hr}h`;
}

function jobRunDuration(job) {
  if (!job.created_at) return null;
  return (job.finished_at ?? Date.now()) - job.created_at;
}

function fmtValue(value, opts = {}) {
  if (value == null || value === '') return '—';
  if (typeof value === 'number') {
    if (opts.decimals != null) return value.toFixed(opts.decimals);
    return value.toLocaleString();
  }
  return String(value);
}

function formatField(key, value) {
  if (value == null || value === '') return '—';
  if (key === 'p99_penalty') return fmtValue(value, { decimals: 3 });
  if (key === 'p50' || key === 'p90' || key === 'p99') {
    return `${fmtValue(value)} ns`;
  }
  if (key === 'latency_ratio') return fmtValue(value, { decimals: 4 });
  return fmtValue(value);
}

function pickStats(obj, keys, { accent = [] } = {}) {
  if (!obj) return [];
  return keys
    .filter((key) => obj[key] != null)
    .map((key) => ({
      key,
      label: FIELD_LABELS[key] ?? key.replace(/_/g, ' '),
      value: formatField(key, obj[key]),
      accent: accent.includes(key),
    }));
}

function usePolling(url) {
  const [data, setData] = useState([]);

  useEffect(() => {
    let active = true;

    const load = async () => {
      try {
        const response = await fetch(url);
        const result = await response.json();
        if (active) setData(result);
      } catch (err) {
        console.error(err);
      }
    };

    load();
    const interval = setInterval(load, POLL_MS);
    return () => {
      active = false;
      clearInterval(interval);
    };
  }, [url]);

  return data;
}

function MetricBlock({ title, stats, pending }) {
  if (!stats.length) return null;

  return (
    <div className="metricBlock">
      <div className="metricBlockTitle">
        {title}
        {pending ? <span className="metricPending">{pending}</span> : null}
      </div>
      <div className="metricGrid">
        {stats.map(({ key, label, value, accent }) => (
          <div key={key} className="metricCell">
            <span className="metricLabel" title={label}>{label}</span>
            <span className={`metricValue ${accent ? 'accent' : ''}`} title={value}>{value}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

function StatusBadge({ status }) {
  const label = (status || 'unknown').replace(/_/g, ' ');
  return <span className={`statusBadge statusBadge-${status || 'unknown'}`}>{label}</span>;
}

const SHUTDOWN_LABELS = {
  graceful: 'Graceful Exit',
  queue_overflow: 'Queue Overflow',
  sla_breach: 'SLA Breach',
  unknown: 'Unknown',
};

function formatShutdownLabel(phase, reason) {
  const label =
    SHUTDOWN_LABELS[reason] ??
    reason.replace(/_/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase());
  const prefix = phase === 1 ? 'Phase1' : 'Phase 2';
  return `${prefix}: ${label}`;
}

function ShutdownTag({ phase, reason }) {
  if (!reason) return null;
  const isFailure = reason !== 'graceful';
  return (
    <span className={`shutdownTag ${isFailure ? 'shutdownTag-warn' : 'shutdownTag-ok'}`}>
      {formatShutdownLabel(phase, reason)}
    </span>
  );
}

function ShutdownFooter({ phase1_metrics, phase2_metrics }) {
  const p1Reason = phase1_metrics?.shutdown_reason;
  const p2Reason = phase2_metrics?.shutdown_reason;
  if (!p1Reason && !p2Reason) return null;

  return (
    <div className="shutdownTags">
      {p1Reason ? <ShutdownTag phase={1} reason={p1Reason} /> : null}
      {p2Reason ? <ShutdownTag phase={2} reason={p2Reason} /> : null}
    </div>
  );
}

function LeaderboardCard({ entry, rank }) {
  return (
    <article className="leaderboardCard">
      <div className="cardHeader">
        <div className="cardIdentity">
          <span className="cardRank">#{rank}</span>
          <span className="cardTeam">{entry.team}</span>
          <code className="jobId" title={entry.best_job_id}>{shortJobId(entry.best_job_id)}</code>
        </div>
        <div className="cardScoreBlock">
          <span className="cardScore">{fmtValue(entry.score_metrics.final_score)}</span>
          {entry.best_score_at ? (
            <span className="jobTime">{prettyTime(entry.best_score_at)}</span>
          ) : null}
        </div>
      </div>

      <div className="metricStack">
        <MetricBlock title="Phase 1" stats={pickStats(entry.phase1_metrics, PHASE1_KEYS, { accent: ['trade_tps'] })} />
        <MetricBlock title="Phase 2" stats={pickStats(entry.phase2_metrics, PHASE2_KEYS, { accent: ['trade_tps'] })} />
        <MetricBlock title="Score" stats={pickStats(entry.score_metrics, SCORE_KEYS, { accent: ['final_score'] })} />
      </div>

      <ShutdownFooter phase1_metrics={entry.phase1_metrics} phase2_metrics={entry.phase2_metrics} />
    </article>
  );
}

function JobMetrics({ job }) {
  if (job.status === 'queued') return null;

  const phase1Pending = job.status === 'phase1_running' || job.status === 'compiling' ? 'running' : null;
  const phase2Pending = job.status === 'phase2_running' ? 'running' : null;
  const scoreStats = pickStats(job.score_metrics, SCORE_KEYS, { accent: ['final_score'] });

  return (
    <div className="metricStack">
      <MetricBlock title="Phase 1" stats={pickStats(job.phase1_metrics, PHASE1_KEYS)} pending={phase1Pending} />
      <MetricBlock title="Phase 2" stats={pickStats(job.phase2_metrics, PHASE2_KEYS)} pending={phase2Pending} />
      {scoreStats.length > 0 ? <MetricBlock title="Score" stats={scoreStats} /> : null}
    </div>
  );
}

function Leaderboard() {
  const leaderboard = usePolling('/api/leaderboard');

  return (
    <div className="panel leaderboardPanel">
      <div className="panelHeader">
        <h2>Leaderboard</h2>
      </div>
      <div className="panelBody scrollPanel">
        {leaderboard.length === 0 ? (
          <div className="emptyState">No submissions yet. Deploy an engine to begin.</div>
        ) : (
          <div className="leaderboardList">
            {leaderboard.map((entry, index) => (
              <LeaderboardCard key={entry.team} entry={entry} rank={index + 1} />
            ))}
          </div>
        )}
      </div>
    </div>
  );
}

function Jobs() {
  const jobs = usePolling('/api/jobs');

  return (
    <div className="panel jobsPanel">
      <div className="panelHeader">
        <h2>Recent Runs</h2>
        <span className="panelHint">{jobs.length} job{jobs.length === 1 ? '' : 's'}</span>
      </div>
      <div className="jobsList scrollPanel">
        {jobs.length === 0 ? (
          <div className="emptyState">No jobs yet.</div>
        ) : (
          jobs.map((j) => (
            <article key={j.job_id} className={`jobCard status-${j.status}`}>
              <div className="cardHeader">
                <div className="cardIdentity">
                  <span className="cardTeam">{j.team}</span>
                  <code className="jobId" title={j.job_id}>{shortJobId(j.job_id)}</code>
                </div>
                <div className="cardMeta">
                  <StatusBadge status={j.status} />
                  <span className="jobTime">{prettyTime(j.created_at)}</span>
                </div>
              </div>

              <JobMetrics job={j} />

              <div className="jobCardFooter">
                <ShutdownFooter phase1_metrics={j.phase1_metrics} phase2_metrics={j.phase2_metrics} />
                {j.created_at ? (
                  <span className="jobDuration">
                    {j.finished_at ? 'Total ' : 'Elapsed '}
                    {formatDuration(jobRunDuration(j))}
                  </span>
                ) : null}
              </div>

              {j.error ? <div className="jobError">{j.error}</div> : null}
            </article>
          ))
        )}
      </div>
    </div>
  );
}

export default function App() {
  const [teamName, setTeamName] = useState('Team Alpha');
  const [file, setFile] = useState(null);
  const [status, setStatus] = useState('Ready to submit');

  const handleUpload = async () => {
    if (!file) {
      alert('Please select a .cpp file first!');
      return;
    }

    setStatus('Uploading…');

    const formData = new FormData();
    formData.append('team_name', teamName);
    formData.append('code_file', file);

    try {
      const response = await fetch('/api/submit', { method: 'POST', body: formData });
      const data = await response.json();
      setStatus(data.success ? `Queued · ${shortJobId(data.job_id)}` : 'Upload failed');
    } catch {
      setStatus('Cannot reach API');
    }
  };

  return (
    <div className="app">
      <header className="appHeader">
        <h1 className="title">Trading Arena</h1>
        <p className="subtitle">Submit your matching engine · survive the bot swarm · climb the leaderboard</p>
      </header>

      <div className="topGrid">
        <section className="deployPanel panel">
          <h2 className="sectionTitle">Deploy Engine</h2>

          <label className="field">
            <span className="fieldLabel">Team name</span>
            <input
              type="text"
              value={teamName}
              onChange={(e) => setTeamName(e.target.value)}
              className="input"
              placeholder="Your team"
            />
          </label>

          <label className="field">
            <span className="fieldLabel">C++ source</span>
            <input
              type="file"
              accept=".cpp"
              onChange={(e) => setFile(e.target.files[0] || null)}
              className="input fileInput"
            />
            {file ? <span className="fileName">{file.name}</span> : null}
          </label>

          <button type="button" onClick={handleUpload} className="submitBtn">
            Submit engine
          </button>

          <div className="statusLine">
            <span className={`statusDot ${status.startsWith('Queued') ? 'live' : ''}`} />
            <span>{status}</span>
          </div>
        </section>

        <Leaderboard />
      </div>

      <Jobs />
    </div>
  );
}

import React, { useState, useEffect } from 'react';
import './App.css';

const POLL_MS = 3000;

const FIELD_LABELS = {
  orders_processed: 'Orders',
  trades_executed: 'Trades',
  max_ops: 'Max ops',
  processing_duration_ns: 'Processing time',
  peak_multiplier: 'Peak level',
  p50: 'p50',
  p90: 'p90',
  p99: 'p99',
  final_score: 'Score',
  p99_penalty: 'p99 penalty',
};

const PHASE1_KEYS = ['orders_processed', 'trades_executed', 'max_ops', 'processing_duration_ns'];
const PHASE2_KEYS = ['peak_multiplier', 'max_ops', 'p50', 'p90', 'p99'];
const SCORE_KEYS = ['final_score', 'max_ops', 'p99_penalty'];

function shortJobId(id) {
  if (!id) return '—';
  return id.length <= 16 ? id : `${id.slice(0, 10)}…${id.slice(-6)}`;
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
  return sec ? `${min}m ${sec}s` : `${min}m`;
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

function formatDurationFromNs(ns) {
  if (ns == null || ns < 0) return '—';
  const ms = ns / 1_000_000;
  if (ms < 1000) return `${ms.toFixed(ms >= 100 ? 0 : 1)} ms`;
  const sec = ms / 1000;
  if (sec < 60) return `${sec.toFixed(sec >= 10 ? 1 : 2)} s`;
  const min = Math.floor(sec / 60);
  const remSec = sec % 60;
  return remSec >= 1 ? `${min}m ${Math.round(remSec)}s` : `${min}m`;
}

function formatField(key, value) {
  if (value == null || value === '') return '—';
  if (key === 'p99_penalty') return fmtValue(value, { decimals: 3 });
  if (key === 'peak_multiplier') return `${fmtValue(value)}×`;
  if (key === 'processing_duration_ns') return formatDurationFromNs(value);
  if (key === 'max_ops') {
    return `${fmtValue(value)} ops`;
  }
  if (key === 'p50' || key === 'p90' || key === 'p99') {
    return `${fmtValue(value)} ns`;
  }
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
        if (active) setData(await response.json());
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
  if (!stats.length && !pending) return null;
  const latencyKeys = ['p50', 'p90', 'p99'];
  const latencyStats = stats.filter((s) => latencyKeys.includes(s.key));
  const normalStats = stats.filter((s) => !latencyKeys.includes(s.key));

  return (
    <div className="metricBlock">
      <div className="metricBlockTitle">
        {title}
        {pending ? <span className="metricPending">{pending}</span> : null}
      </div>
      {normalStats.length > 0 && (
        <div className="metricGrid">
          {normalStats.map(({ key, label, value, accent }) => (
            <div key={key} className="metricCell">
              <span className="metricLabel">{label}</span>
              <span className={`metricValue ${accent ? 'accent' : ''}`}>{value}</span>
            </div>
          ))}
        </div>
      )}
      {latencyStats.length > 0 && (
        <div className="metricGrid latencyRow" style={{ marginTop: normalStats.length > 0 ? '6px' : '0' }}>
          {latencyStats.map(({ key, label, value, accent }) => (
            <div key={key} className="metricCell">
              <span className="metricLabel">{label}</span>
              <span className={`metricValue ${accent ? 'accent' : ''}`}>{value}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

function StatusBadge({ status }) {
  const isRunningPhase2 = (status || '').startsWith('phase2_running');
  const badgeClass = isRunningPhase2 ? 'statusBadge-phase2_running' : `statusBadge-${status || 'unknown'}`;
  return (
    <span className={`statusBadge ${badgeClass}`}>
      {(status || 'unknown').replace(/_/g, ' ')}
    </span>
  );
}

const SHUTDOWN_LABELS = {
  graceful: 'Graceful exit',
  queue_overflow: 'Queue overflow',
  sla_breach: 'SLA breach',
  health_breach: 'Health breach',
  health_breach_queue_depth: 'Health breach: queue depth',
  health_breach_latency: 'Health breach: latency',
  health_breach_queue_depth_and_latency: 'Health breach: queue depth & latency',
};

function ShutdownTag({ phase, reason }) {
  if (!reason) return null;
  const label = SHUTDOWN_LABELS[reason];
  if (!label) return null;
  return (
    <span className={`shutdownTag ${reason === 'graceful' ? 'shutdownTag-ok' : 'shutdownTag-warn'}`}>
      Phase {phase}: {label}
    </span>
  );
}

function ShutdownFooter({ phase1_metrics, phase2_metrics }) {
  const p1 = phase1_metrics?.shutdown_reason;
  const p2 = phase2_metrics?.shutdown_reason;
  if (!p1 && !p2) return null;
  return (
    <div className="shutdownTags">
      {p1 ? <ShutdownTag phase={1} reason={p1} /> : null}
      {p2 ? <ShutdownTag phase={2} reason={p2} /> : null}
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
          <code className="jobId">{shortJobId(entry.best_job_id)}</code>
        </div>
        <span className="cardScore">{fmtValue(entry.score_metrics?.final_score)}</span>
      </div>
      <div className="metricStack">
        <MetricBlock
          title="Phase 1"
          stats={pickStats(entry.phase1_metrics, PHASE1_KEYS, { accent: ['max_ops'] })}
        />
        <MetricBlock
          title="Phase 2"
          stats={pickStats(entry.phase2_metrics, PHASE2_KEYS, { accent: ['max_ops'] })}
        />
      </div>
      <ShutdownFooter phase1_metrics={entry.phase1_metrics} phase2_metrics={entry.phase2_metrics} />
    </article>
  );
}

function JobMetrics({ job }) {
  if (job.status === 'queued') return null;
  const phase1Pending = job.status === 'phase1_running' || job.status === 'compiling' ? 'running' : null;
  const phase2Pending = (job.status || '').startsWith('phase2_running')
    ? (job.status.includes('(')
        ? job.status.substring(job.status.indexOf('(') + 1, job.status.indexOf(')'))
        : 'running')
    : null;
  const scoreStats = pickStats(job.score_metrics, SCORE_KEYS, { accent: ['final_score'] });

  return (
    <div className="metricStack">
      <MetricBlock
        title="Phase 1"
        stats={pickStats(job.phase1_metrics, PHASE1_KEYS, { accent: ['max_ops'] })}
        pending={phase1Pending}
      />
      <MetricBlock
        title="Phase 2"
        stats={pickStats(job.phase2_metrics, PHASE2_KEYS, { accent: ['max_ops'] })}
        pending={phase2Pending}
      />
      {scoreStats.length > 0 ? <MetricBlock title="Score" stats={scoreStats} /> : null}
    </div>
  );
}

function Leaderboard() {
  const leaderboard = usePolling('/api/leaderboard');
  return (
    <div className="panel leaderboardPanel">
      <div className="panelHeader"><h2>Leaderboard</h2></div>
      <div className="panelBody scrollPanel">
        {leaderboard.length === 0 ? (
          <div className="emptyState">No submissions yet.</div>
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
            <article key={j.job_id} className={`jobCard status-${(j.status || 'unknown').split(' ')[0]}`}>
              <div className="cardHeader">
                <div className="cardIdentity">
                  <span className="cardTeam">{j.team}</span>
                  <code className="jobId">{shortJobId(j.job_id)}</code>
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
  const [status, setStatus] = useState('Ready');

  const handleUpload = async () => {
    if (!file) {
      alert('Select a .cpp file first.');
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
        <p className="subtitle">Phase 1: Correctness · Phase 2 : Benchmark (Max Throughput)</p>
      </header>
      <div className="topGrid">
        <section className="deployPanel panel">
          <h2 className="sectionTitle">Deploy Engine</h2>
          <label className="field">
            <span className="fieldLabel">Team</span>
            <input type="text" value={teamName} onChange={(e) => setTeamName(e.target.value)} className="input" />
          </label>
          <label className="field">
            <span className="fieldLabel">C++ source</span>
            <input type="file" accept=".cpp" onChange={(e) => setFile(e.target.files[0] || null)} className="input fileInput" />
          </label>
          <button type="button" onClick={handleUpload} className="submitBtn">Submit</button>
          <div className="statusLine"><span className={`statusDot ${status.startsWith('Queued') ? 'live' : ''}`} /><span>{status}</span></div>
        </section>
        <Leaderboard />
      </div>
      <Jobs />
    </div>
  );
}

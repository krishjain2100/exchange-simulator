import React, { useState, useEffect } from 'react';
import './App.css';

function Leaderboard() {
  const [leaderboard, setLeaderboard] = useState([]);

  useEffect(() => {
    // Poll the backend every 3 seconds for live updates
    const fetchLeaderboard = async () => {
      try {
        const response = await fetch('/api/leaderboard');
        const data = await response.json();
        setLeaderboard(data);
      } catch (error) {
        console.error("Failed to fetch leaderboard", error);
      }
    };

    fetchLeaderboard();
    const interval = setInterval(fetchLeaderboard, 3000);
    return () => clearInterval(interval);
  }, []);

  return (
    <div className="panel leaderboard">
      <div className="leaderboardBody">
        <table className="leaderboardTable">
          <thead>
            <tr>
              <th>Rank</th>
              <th>Team</th>
              <th className="numeric">Score</th>
              <th className="numeric">p50</th>
              <th className="numeric">p90</th>
              <th className="numeric">p99</th>
            </tr>
          </thead>
          <tbody>
            {leaderboard.length === 0 ? (
              <tr>
                <td colSpan="6" className="emptyMessage">
                  No submissions yet. Awaiting bot fleet data...
                </td>
              </tr>
            ) : (
              leaderboard.map((team, index) => (
                <tr key={team.team} className="tableRow">
                  <td className="rank">#{index + 1}</td>
                  <td className="team">{team.team}</td>
                  <td className="numeric font-mono">{team.composite}</td>
                  <td className="numeric font-mono">{team.p50} ns</td>
                  <td className="numeric font-mono">{team.p90} ns</td>
                  <td className="numeric font-mono">{team.p99} ns</td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}


function Jobs() {
  const [jobs, setJobs] = useState([]);

  useEffect(() => {
    const fetchJobs = async () => {
      try {
        const res = await fetch('/api/jobs');
        const data = await res.json();
        setJobs(data);
      } catch (err) {
        console.error('Failed to fetch jobs', err);
      }
    };

    fetchJobs();
    const t = setInterval(fetchJobs, 3000);
    return () => clearInterval(t);
  }, []);

  const prettyTime = (ms) => ms ? new Date(ms).toLocaleTimeString() : '-';

  return (
    <div className="panel">
      <h3>Recent Jobs</h3>
      <div className="jobsList">
        {jobs.length === 0 ? (
          <div className="emptyMessage">No recent jobs.</div>
        ) : (
          jobs.map((j) => (
            <div key={j.job_id} className={`jobRow status-${j.status}`}>
              <div className="jobMeta">
                <div className="jobId">{j.job_id}</div>
                <div className="jobTeam">{j.team}</div>
              </div>
              <div className="jobStatus">{j.status}</div>
              <div className="jobTimes">{prettyTime(j.created_at)} → {prettyTime(j.finished_at)}</div>
              {j.error ? <div className="jobError">{j.error}</div> : null}
            </div>
          ))
        )}
      </div>
    </div>
  );
}


export default function App() {
  const [teamName, setTeamName] = useState('Team Alpha');
  const [file, setFile] = useState(null);
  const [status, setStatus] = useState('Awaiting Submission...');

  const handleUpload = async () => {
    if (!file) {
      alert('Please select a .cpp file first!');
      return;
    }

    setStatus('Uploading and Queueing Job...');

    const formData = new FormData();
    formData.append('team_name', teamName);
    formData.append('code_file', file);

    try {
      const response = await fetch('/api/submit', {
        method: 'POST',
        body: formData,
      });

      const data = await response.json();

      if (data.success) {
        setStatus(`Job Queued! ID: ${data.job_id}. Waiting for worker...`);
      } else {
        setStatus('Upload failed.');
      }
    } catch (err) {
      setStatus('Error: Could not connect to API.');
    }
  };

  return (
    <div className="container">
      <h1 className="title">Trading Arena</h1>

      <div className="columns">

        {/* LEFT COLUMN: UPLOAD FORM */}
        <div className="column">
          <h2>1. Deploy Engine</h2>
          <div className="panel">

            <div className="field">
              <label className="formLabel">Team Name:</label>
              <input
                type="text"
                value={teamName}
                onChange={(e) => setTeamName(e.target.value)}
                className="inputFull"
              />
            </div>

            <div className="field">
              <label className="formLabel">C++ Source Code:</label>
              <input
                type="file"
                accept=".cpp"
                onChange={(e) => setFile(e.target.files[0])}
                className="inputFull"
              />
            </div>

            <button onClick={handleUpload} className="buttonPrimary">Submit</button>

            <div className="statusBox mt-4">
              <span className="muted">System Status:</span>
              <div className="font-mono mt-1 text-sm">{status}</div>
            </div>
            <div className="jobsWrapper">
              <Jobs />
            </div>
          </div>
        </div>

        {/* RIGHT COLUMN: LEADERBOARD */}
        <div className="column">
          <h2>2. Live Leaderboard (Telemetry)</h2>
          <Leaderboard />
        </div>

      </div>
    </div>
  );
}
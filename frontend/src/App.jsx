import { useState, useEffect } from 'react';
import './App.css';

export default function App() {
  const [teamName, setTeamName] = useState('Team Alpha');
  const [file, setFile] = useState(null);
  const [status, setStatus] = useState('Awaiting Submission...');
  const [leaderboard, setLeaderboard] = useState([]);

  // Fetch the leaderboard from Redis/Express
  const fetchLeaderboard = async () => {
    try {
      const response = await fetch('/api/leaderboard');
      const data = await response.json();
      if (data.success) {
        setLeaderboard(data.leaderboard);
      }
    } catch (err) {
      console.error('Failed to fetch leaderboard', err);
    }
  };

  // Poll the leaderboard every 3 seconds
  useEffect(() => {
    fetchLeaderboard();
    const intervalId = setInterval(fetchLeaderboard, 3000);
    return () => clearInterval(intervalId); // Cleanup on unmount
  }, []);

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

            <div className="statusBox">
              <span className="muted">System Status:</span>
              <div>{status}</div>
            </div>
          </div>
        </div>

        {/* RIGHT COLUMN: LEADERBOARD */}
        <div className="column">
          <h2>2. Live Leaderboard</h2>
          <div className="panel leaderboard">

            {/* Table Header */}
            <div className="headerRow">
              <div className="rank">Rank</div>
              <div className="teamCol">Team</div>
              <div className="latencyCol">Algo Latency</div>
            </div>

            {/* Table Body */}
            <div className="leaderboardBody">
              {leaderboard.length === 0 ? (
                <div className="emptyMessage">No submissions yet. Be the first!</div>
              ) : (
                leaderboard.map((entry) => (
                  <div key={entry.team} className="row">
                    <div className="rank">#{entry.rank}</div>
                    <div className="teamCol">{entry.team}</div>
                    <div className="latencyCol">{entry.latency_ns} ns</div>
                  </div>
                ))
              )}
            </div>

          </div>
        </div>

      </div>
    </div>
  );
}
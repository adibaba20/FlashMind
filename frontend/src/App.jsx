import { useEffect, useState } from "react";
import axios from "axios";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  BarChart,
  Bar,
} from "recharts";
import "./App.css";

const API = "http://localhost:5036";

function App() {
  const [telemetry, setTelemetry] = useState([]);
  const [mlPredictions, setMlPredictions] = useState([]);
  const [loading, setLoading] = useState(true);
  const [running, setRunning] = useState(false);
  const [simulationOutput, setSimulationOutput] = useState("");

  useEffect(() => {
    loadDashboard();
  }, []);

  const loadDashboard = async () => {
    try {
      setLoading(true);

      const telemetryResponse = await axios.get(`${API}/api/telemetry`);

      const data = telemetryResponse.data.map((row) => ({
        tick: Number(row.tick),
        block_id: Number(row.block_id),
        erase_count: Number(row.erase_count),
        rber: Number(row.rber),
        error_bits: Number(row.error_bits),
        uncorrectable: Number(row.uncorrectable),
        temp_c: Number(row.temp_c),
        valid_pages: Number(row.valid_pages),
        invalid_pages: Number(row.invalid_pages),
      }));

      setTelemetry(data);

      try {
        const mlResponse = await axios.get(`${API}/api/ml/predictions`);
        setMlPredictions(mlResponse.data.blocks || []);
      } catch (mlError) {
        console.error("ML prediction loading failed:", mlError);
        setMlPredictions([]);
      }
    } catch (error) {
      console.error("Dashboard loading failed:", error);
    } finally {
      setLoading(false);
    }
  };

  const runSimulation = async () => {
    try {
      setRunning(true);
      setSimulationOutput("");

      const response = await axios.get(`${API}/api/simulator/run`);

      setSimulationOutput(response.data.output || "");

      await loadDashboard();
    } catch (error) {
      console.error("Simulation failed:", error);
      setSimulationOutput(
        "Unable to run simulation. Make sure the ASP.NET Core API is running on port 5036."
      );
    } finally {
      setRunning(false);
    }
  };

  /* =========================
     LATEST BLOCK DATA
  ========================= */

  const latestByBlock = [];

  for (let block = 0; block < 6; block++) {
    const rows = telemetry.filter((x) => x.block_id === block);

    if (rows.length > 0) {
      latestByBlock.push(rows[rows.length - 1]);
    }
  }

  /* =========================
     TELEMETRY STATISTICS
  ========================= */

  const totalErrors = telemetry.reduce(
    (sum, row) => sum + row.uncorrectable,
    0
  );

  const maxRber =
    telemetry.length > 0
      ? Math.max(...telemetry.map((x) => x.rber))
      : 0;

  const maxErase =
    latestByBlock.length > 0
      ? Math.max(...latestByBlock.map((x) => x.erase_count))
      : 0;

  /* =========================
     ML STATISTICS
  ========================= */

  const highRiskBlocks = mlPredictions.filter(
    (block) => block.risk === "HIGH"
  ).length;

  const mediumRiskBlocks = mlPredictions.filter(
    (block) => block.risk === "MEDIUM"
  ).length;

  const lowRiskBlocks = mlPredictions.filter(
    (block) => block.risk === "LOW"
  ).length;

  const riskAwareGC = mlPredictions.filter(
    (block) => block.recommendation === "RISK-AWARE GC"
  ).length;

  const priorityMonitoring = mlPredictions.filter(
    (block) => block.recommendation === "PRIORITIZE MONITORING"
  ).length;

  const monitorClosely = mlPredictions.filter(
    (block) => block.recommendation === "MONITOR CLOSELY"
  ).length;

  const normalOperation = mlPredictions.filter(
    (block) => block.recommendation === "NORMAL OPERATION"
  ).length;

  /* =========================
     CHART DATA
  ========================= */

  const rberData = telemetry
    .filter((_, index) => index % 6 === 0)
    .map((row) => ({
      tick: row.tick,
      rber: row.rber,
    }));

  const riskData = [
    {
      name: "HIGH",
      count: highRiskBlocks,
    },
    {
      name: "MEDIUM",
      count: mediumRiskBlocks,
    },
    {
      name: "LOW",
      count: lowRiskBlocks,
    },
  ];

  return (
    <div className="app">

      {/* =========================
          HEADER
      ========================= */}

      <header className="header">

        <div className="brand">
          <h1>FlashMind</h1>

          <p>
            Intelligent Flash Storage Monitoring &amp; Prediction
          </p>
        </div>

        <div className="status">
          <span className="status-dot"></span>
          SYSTEM ONLINE
        </div>

      </header>

      <main>

        {/* =========================
            HERO
        ========================= */}

        <section className="hero">

          <div className="hero-content">

            <span className="hero-label">
              FLASH STORAGE INTELLIGENCE
            </span>

            <h2>
              SSD Health &amp; FTL Analytics
            </h2>

            <p>
              Monitor NAND wear, detect degradation, predict failures,
              and optimize Flash Translation Layer decisions using
              machine learning.
            </p>

          </div>

          <button
            onClick={runSimulation}
            disabled={running}
            className="run-button"
          >
            {running
              ? "Running Simulation..."
              : "▶ Run FTL Simulation"}
          </button>

        </section>

        {/* =========================
            TELEMETRY CARDS
        ========================= */}

        <section className="cards">

          <div className="card">
            <span>Telemetry Rows</span>
            <strong>{telemetry.length}</strong>
            <small>Recorded snapshots</small>
          </div>

          <div className="card">
            <span>Max Erase Count</span>
            <strong>{maxErase}</strong>
            <small>Highest block wear</small>
          </div>

          <div className="card">
            <span>Peak RBER</span>
            <strong>{maxRber.toFixed(6)}</strong>
            <small>Raw bit error rate</small>
          </div>

          <div className="card danger">
            <span>Uncorrectable Events</span>
            <strong>{totalErrors}</strong>
            <small>Observed telemetry events</small>
          </div>

        </section>

        {/* =========================
            ML OVERVIEW
        ========================= */}

        <section className="cards ml-cards">

          <div className="card">
            <span>ML Monitored Blocks</span>
            <strong>{mlPredictions.length}</strong>
            <small>Blocks analyzed by ML</small>
          </div>

          <div className="card danger">
            <span>High Risk Blocks</span>
            <strong>{highRiskBlocks}</strong>
            <small>ML failure prediction</small>
          </div>

          <div className="card warning">
            <span>Medium Risk Blocks</span>
            <strong>{mediumRiskBlocks}</strong>
            <small>ML warning level</small>
          </div>

          <div className="card">
            <span>ML Status</span>
            <strong className="active-text">
              {mlPredictions.length > 0 ? "ACTIVE" : "WAITING"}
            </strong>
            <small>XGBoost prediction engine</small>
          </div>

        </section>

        {/* =========================
            DECISION CARDS
        ========================= */}

        <section className="decision-grid">

          <div className="decision-card gc">
            <div className="decision-icon">⚡</div>

            <div>
              <span>Risk-Aware GC</span>
              <strong>{riskAwareGC}</strong>
              <small>Priority garbage collection</small>
            </div>
          </div>

          <div className="decision-card monitoring">
            <div className="decision-icon">👁</div>

            <div>
              <span>Priority Monitoring</span>
              <strong>{priorityMonitoring}</strong>
              <small>Blocks requiring attention</small>
            </div>
          </div>

          <div className="decision-card warning">
            <div className="decision-icon">⚠</div>

            <div>
              <span>Monitor Closely</span>
              <strong>{monitorClosely}</strong>
              <small>Warning-level blocks</small>
            </div>
          </div>

          <div className="decision-card healthy">
            <div className="decision-icon">✓</div>

            <div>
              <span>Normal Operation</span>
              <strong>{normalOperation}</strong>
              <small>Normal operating blocks</small>
            </div>
          </div>

        </section>

        {/* =========================
            CHARTS
        ========================= */}

        <section className="grid">

          <div className="panel large">

            <div className="panel-title">

              <div>
                <h3>RBER Trend</h3>
                <p>NAND raw bit error rate over simulation</p>
              </div>

              <span>Bit Error Rate</span>

            </div>

            <ResponsiveContainer width="100%" height={320}>

              <LineChart data={rberData}>

                <CartesianGrid
                  strokeDasharray="3 3"
                  stroke="#283653"
                />

                <XAxis
                  dataKey="tick"
                  tick={{ fill: "#8fa3cf" }}
                  label={{
                    value: "Simulation Tick",
                    position: "insideBottom",
                    offset: -5,
                    fill: "#8fa3cf",
                  }}
                />

                <YAxis
                  tick={{ fill: "#8fa3cf" }}
                />

                <Tooltip />

                <Line
                  type="monotone"
                  dataKey="rber"
                  name="RBER"
                  stroke="#5d8bff"
                  strokeWidth={3}
                  dot={false}
                />

              </LineChart>

            </ResponsiveContainer>

          </div>

          <div className="panel">

            <div className="panel-title">

              <div>
                <h3>Block Wear</h3>
                <p>NAND erase cycles by block</p>
              </div>

              <span>Erase Count</span>

            </div>

            <ResponsiveContainer width="100%" height={320}>

              <BarChart data={latestByBlock}>

                <CartesianGrid
                  strokeDasharray="3 3"
                  stroke="#283653"
                />

                <XAxis
                  dataKey="block_id"
                  tick={{ fill: "#8fa3cf" }}
                />

                <YAxis
                  tick={{ fill: "#8fa3cf" }}
                />

                <Tooltip />

                <Bar
                  dataKey="erase_count"
                  name="Erase Count"
                  fill="#5d8bff"
                  radius={[5, 5, 0, 0]}
                />

              </BarChart>

            </ResponsiveContainer>

          </div>

        </section>

        {/* =========================
            RISK DISTRIBUTION
        ========================= */}

        <section className="grid">

          <div className="panel">

            <div className="panel-title">

              <div>
                <h3>ML Risk Distribution</h3>
                <p>Current block risk classification</p>
              </div>

              <span>{mlPredictions.length} Blocks</span>

            </div>

            <ResponsiveContainer width="100%" height={300}>

              <BarChart data={riskData}>

                <CartesianGrid
                  strokeDasharray="3 3"
                  stroke="#283653"
                />

                <XAxis
                  dataKey="name"
                  tick={{ fill: "#8fa3cf" }}
                />

                <YAxis
                  allowDecimals={false}
                  tick={{ fill: "#8fa3cf" }}
                />

                <Tooltip />

                <Bar
                  dataKey="count"
                  name="Blocks"
                  fill="#5d8bff"
                  radius={[5, 5, 0, 0]}
                />

              </BarChart>

            </ResponsiveContainer>

          </div>

          <div className="panel decision-summary">

            <div className="panel-title">

              <div>
                <h3>ML Decision Summary</h3>
                <p>FTL risk-aware recommendations</p>
              </div>

              <span>XGBoost</span>

            </div>

            <div className="summary-row">
              <span>⚡ RISK-AWARE GC</span>
              <strong>{riskAwareGC}</strong>
            </div>

            <div className="summary-row">
              <span>👁 PRIORITIZE MONITORING</span>
              <strong>{priorityMonitoring}</strong>
            </div>

            <div className="summary-row">
              <span>⚠ MONITOR CLOSELY</span>
              <strong>{monitorClosely}</strong>
            </div>

            <div className="summary-row">
              <span>✓ NORMAL OPERATION</span>
              <strong>{normalOperation}</strong>
            </div>

          </div>

        </section>

        {/* =========================
            ML PREDICTIONS
        ========================= */}

        <section className="panel">

          <div className="panel-title">

            <div>
              <h3>ML Predictions</h3>
              <p>
                XGBoost failure classification + RUL estimation
              </p>
            </div>

            <span>LIVE ML OUTPUT</span>

          </div>

          <div className="table-wrapper">

            <table>

              <thead>

                <tr>
                  <th>Block</th>
                  <th>Erase Count</th>
                  <th>Failure Probability</th>
                  <th>Predicted RUL</th>
                  <th>Prediction</th>
                  <th>Risk</th>
                  <th>Recommendation</th>
                </tr>

              </thead>

              <tbody>

                {mlPredictions.map((block) => (

                  <tr key={block.block_id}>

                    <td>
                      <strong>Block {block.block_id}</strong>
                    </td>

                    <td>
                      {block.erase_count}
                    </td>

                    <td>
                      {(block.failure_probability * 100).toFixed(2)}%
                    </td>

                    <td>
                      {block.predicted_rul.toFixed(1)} cycles
                    </td>

                    <td>

                      {block.failure_prediction ? (
                        <span className="bad">
                          ⚠ FAILURE PREDICTED
                        </span>
                      ) : (
                        <span className="good">
                          ✓ NO FAILURE
                        </span>
                      )}

                    </td>

                    <td>

                      <span
                        className={
                          block.risk === "HIGH"
                            ? "risk-high"
                            : block.risk === "MEDIUM"
                              ? "risk-medium"
                              : "risk-low"
                        }
                      >
                        {block.risk}
                      </span>

                    </td>

                    <td>

                      {block.recommendation === "RISK-AWARE GC" && (
                        <span className="recommendation gc-text">
                          ⚡ RISK-AWARE GC
                        </span>
                      )}

                      {block.recommendation === "PRIORITIZE MONITORING" && (
                        <span className="recommendation monitoring-text">
                          👁 PRIORITIZE MONITORING
                        </span>
                      )}

                      {block.recommendation === "MONITOR CLOSELY" && (
                        <span className="recommendation warning-text">
                          ⚠ MONITOR CLOSELY
                        </span>
                      )}

                      {block.recommendation === "NORMAL OPERATION" && (
                        <span className="recommendation healthy-text">
                          ✓ NORMAL OPERATION
                        </span>
                      )}

                    </td>

                  </tr>

                ))}

              </tbody>

            </table>

          </div>

        </section>

        {/* =========================
            BLOCK HEALTH
        ========================= */}

        <section className="panel">

          <div className="panel-title">

            <div>
              <h3>Block Health</h3>
              <p>Latest telemetry snapshot</p>
            </div>

            <span>NAND STATUS</span>

          </div>

          <div className="table-wrapper">

            <table>

              <thead>

                <tr>
                  <th>Block</th>
                  <th>Erase Count</th>
                  <th>RBER</th>
                  <th>Temperature</th>
                  <th>Error Bits</th>
                  <th>Status</th>
                </tr>

              </thead>

              <tbody>

                {latestByBlock.map((block) => (

                  <tr key={block.block_id}>

                    <td>
                      <strong>Block {block.block_id}</strong>
                    </td>

                    <td>
                      {block.erase_count}
                    </td>

                    <td>
                      {block.rber.toFixed(6)}
                    </td>

                    <td>
                      {block.temp_c}°C
                    </td>

                    <td>
                      {block.error_bits}
                    </td>

                    <td>

                      {block.uncorrectable ? (
                        <span className="bad">
                          ⚠ FAILURE
                        </span>
                      ) : (
                        <span className="good">
                          ✓ HEALTHY
                        </span>
                      )}

                    </td>

                  </tr>

                ))}

              </tbody>

            </table>

          </div>

        </section>

        {/* =========================
            SIMULATION OUTPUT
        ========================= */}

        {simulationOutput && (

          <section className="panel">

            <div className="panel-title">

              <div>
                <h3>Latest Simulation Output</h3>
                <p>C-based FTL simulator response</p>
              </div>

              <span>SIMULATION LOG</span>

            </div>

            <pre>{simulationOutput}</pre>

          </section>

        )}

      </main>

      {/* =========================
          FOOTER
      ========================= */}

      <footer>
        FlashMind • C FTL • NAND Degradation • Python ML • XGBoost • ASP.NET Core • React
      </footer>

    </div>
  );
}

export default App;
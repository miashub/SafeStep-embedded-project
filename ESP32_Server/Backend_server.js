const express = require("express");
const cors = require("cors");
const fs = require("fs");
const path = require("path");
const nodemailer = require("nodemailer");

const app = express();
const PORT = 5000;


const CAREGIVER_DASHBOARD_URL = "http://10.158.215.137/";

app.use(cors());
app.use(express.json());
app.use(express.raw({ type: "application/octet-stream", limit: "10mb" }));
app.use("/uploads", express.static(path.join(__dirname, "uploads")));

const uploadDir = path.join(__dirname, "uploads");
if (!fs.existsSync(uploadDir)) {
  fs.mkdirSync(uploadDir);
}

let latestCapture = null;
let latestEscalation = null;

const CAREGIVER_EMAIL = "fathimas0207@gmail.com";

const SMTP_HOST = "smtp.gmail.com";
const SMTP_PORT = 587;
const SMTP_SECURE = false;
const SMTP_USER = "myouhsk@gmail.com";
const SMTP_PASS = "mtgo dhdv exvt gext";

const EMAIL_FROM = `"SafeStep Alert System" <${SMTP_USER}>`;

const transporter = nodemailer.createTransport({
  host: SMTP_HOST,
  port: SMTP_PORT,
  secure: SMTP_SECURE,
  auth: {
    user: SMTP_USER,
    pass: SMTP_PASS,
  },
});

function getBaseUrl(req) {
  return `${req.protocol}://${req.get("host")}`;
}

function escapeHtml(value = "") {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function formatDateTime(value) {
  try {
    return new Date(value).toLocaleString();
  } catch {
    return value || "-";
  }
}

function loadLatestCaptureFromDisk(req) {
  const files = fs
    .readdirSync(uploadDir)
    .filter((f) => /\.(jpg|jpeg|png)$/i.test(f))
    .map((filename) => {
      const filepath = path.join(uploadDir, filename);
      const stat = fs.statSync(filepath);
      return {
        filename,
        filepath,
        mtimeMs: stat.mtimeMs,
        timestamp: stat.mtime.toISOString(),
      };
    })
    .sort((a, b) => b.mtimeMs - a.mtimeMs);

  if (files.length === 0) {
    return null;
  }

  const latest = files[0];
  const baseUrl = getBaseUrl(req);

  return {
    filename: latest.filename,
    imageUrl: `/uploads/${latest.filename}`,
    fullUrl: `${baseUrl}/uploads/${latest.filename}`,
    latestPageUrl: `${baseUrl}/latest-image`,
    timestamp: latest.timestamp,
  };
}

function loadAllCaptures(req) {
  const baseUrl = getBaseUrl(req);

  return fs
    .readdirSync(uploadDir)
    .filter((f) => /\.(jpg|jpeg|png)$/i.test(f))
    .map((filename) => {
      const filepath = path.join(uploadDir, filename);
      const stat = fs.statSync(filepath);
      return {
        filename,
        mtimeMs: stat.mtimeMs,
        timestamp: stat.mtime.toISOString(),
        imageUrl: `/uploads/${filename}`,
        fullUrl: `${baseUrl}/uploads/${filename}`,
        viewUrl: `${baseUrl}/image/${encodeURIComponent(filename)}`,
      };
    })
    .sort((a, b) => b.mtimeMs - a.mtimeMs);
}

function layoutPage({ title, active = "dashboard", content = "" }) {
  const navDashboardClass = active === "dashboard" ? "nav-link active" : "nav-link";
  const navGalleryClass = active === "gallery" ? "nav-link active" : "nav-link";
  const navLatestClass = active === "latest" ? "nav-link active" : "nav-link";

  return `
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>${escapeHtml(title)}</title>
  <style>
    :root{
      --bg:#020817;
      --bg2:#081226;
      --panel:#0b162b;
      --panel2:#0f1d36;
      --border:#183153;
      --line:#123055;
      --text:#e2e8f0;
      --muted:#8ea3c0;
      --blue:#38bdf8;
      --cyan:#22d3ee;
      --green:#34d399;
      --amber:#fbbf24;
      --red:#fb7185;
      --shadow:0 0 0 1px rgba(56,189,248,0.08), 0 12px 32px rgba(0,0,0,0.35);
    }

    * { box-sizing: border-box; }

    html, body {
      margin: 0;
      padding: 0;
      background:
        radial-gradient(circle at top right, rgba(56,189,248,0.08), transparent 25%),
        radial-gradient(circle at top left, rgba(34,211,238,0.06), transparent 20%),
        linear-gradient(180deg, #020817 0%, #030b18 100%);
      color: var(--text);
      font-family: Arial, Helvetica, sans-serif;
      min-height: 100%;
    }

    a {
      color: var(--blue);
      text-decoration: none;
    }

    a:hover {
      text-decoration: underline;
    }

    .app-shell {
      display: flex;
      min-height: 100vh;
      width: 100%;
    }

    .sidebar {
      width: 280px;
      background: linear-gradient(180deg, rgba(5,14,31,0.98), rgba(8,18,38,0.98));
      border-right: 1px solid rgba(56,189,248,0.12);
      padding: 34px 18px;
      position: sticky;
      top: 0;
      height: 100vh;
    }

    .brand {
      font-size: 26px;
      font-weight: 800;
      color: var(--blue);
      letter-spacing: 0.5px;
      margin-bottom: 26px;
    }

    .nav-title {
      color: var(--muted);
      font-size: 13px;
      letter-spacing: 3px;
      text-transform: uppercase;
      margin-bottom: 18px;
    }

    .nav-links {
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    .nav-link {
      display: block;
      padding: 16px 18px;
      border: 1px solid rgba(56,189,248,0.12);
      border-radius: 16px;
      background: rgba(15,29,54,0.55);
      color: var(--text);
      font-size: 16px;
      font-weight: 600;
      text-decoration: none;
      transition: 0.2s ease;
    }

    .nav-link:hover,
    .nav-link.active {
      border-color: rgba(56,189,248,0.4);
      box-shadow: 0 0 0 1px rgba(56,189,248,0.12), 0 8px 24px rgba(0,0,0,0.25);
      background: rgba(17,35,65,0.9);
      text-decoration: none;
    }

    .main {
      flex: 1;
      padding: 34px 28px 40px;
    }

    .page-head {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 20px;
      margin-bottom: 22px;
      padding-bottom: 18px;
      border-bottom: 1px solid rgba(56,189,248,0.14);
    }

    .page-title {
      margin: 0;
      color: var(--blue);
      font-size: 36px;
      font-weight: 800;
      letter-spacing: 0.4px;
    }

    .page-subtitle {
      margin-top: 8px;
      color: var(--muted);
      font-size: 16px;
      letter-spacing: 1px;
    }

    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 10px;
      padding: 14px 18px;
      border-radius: 999px;
      background: rgba(15,29,54,0.92);
      border: 1px solid rgba(56,189,248,0.14);
      color: var(--text);
      font-size: 15px;
      box-shadow: var(--shadow);
      white-space: nowrap;
    }

    .status-dot {
      width: 10px;
      height: 10px;
      border-radius: 999px;
      background: #ef4444;
      box-shadow: 0 0 12px rgba(239,68,68,0.8);
    }

    .section-label {
      color: var(--muted);
      font-size: 14px;
      letter-spacing: 4px;
      text-transform: uppercase;
      margin: 18px 0 16px;
    }

    .card {
      background: linear-gradient(180deg, rgba(11,22,43,0.96), rgba(9,18,34,0.98));
      border: 1px solid rgba(56,189,248,0.12);
      border-radius: 22px;
      padding: 22px;
      box-shadow: var(--shadow);
    }

    .hero-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(220px, 1fr));
      gap: 18px;
      margin-bottom: 22px;
    }

    .stat-card {
      background: linear-gradient(180deg, rgba(11,22,43,0.98), rgba(15,29,54,0.98));
      border: 1px solid rgba(56,189,248,0.14);
      border-radius: 18px;
      padding: 18px;
      min-height: 128px;
      box-shadow: var(--shadow);
    }

    .stat-label {
      color: var(--muted);
      letter-spacing: 2px;
      font-size: 13px;
      text-transform: uppercase;
      margin-bottom: 14px;
    }

    .stat-value {
      color: var(--blue);
      font-size: 30px;
      font-weight: 800;
      line-height: 1.1;
      margin-bottom: 10px;
    }

    .stat-subtext {
      color: var(--text);
      font-size: 14px;
      opacity: 0.9;
    }

    .image-wrap {
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 260px;
      margin-top: 18px;
      background: rgba(2,8,23,0.75);
      border: 1px solid rgba(56,189,248,0.1);
      border-radius: 18px;
      padding: 16px;
    }

    .image-wrap img {
      display: block;
      max-width: 100%;
      max-height: 70vh;
      width: auto;
      height: auto;
      border-radius: 14px;
      box-shadow: 0 12px 30px rgba(0,0,0,0.45);
      object-fit: contain;
      background: #000;
    }

    .button-row {
      display: flex;
      flex-wrap: wrap;
      gap: 12px;
      margin-top: 18px;
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      min-width: 150px;
      padding: 13px 18px;
      border-radius: 14px;
      border: 1px solid rgba(56,189,248,0.16);
      background: linear-gradient(180deg, #182a48, #11213c);
      color: #fff;
      font-weight: 700;
      font-size: 15px;
      text-decoration: none;
      transition: 0.2s ease;
    }

    .btn:hover {
      transform: translateY(-1px);
      text-decoration: none;
      box-shadow: 0 8px 22px rgba(0,0,0,0.28);
    }

    .btn-primary {
      background: linear-gradient(180deg, #34c9ff, #22a9ec);
      color: #05111f;
      border-color: rgba(56,189,248,0.34);
    }

    .btn-outline {
      background: linear-gradient(180deg, #152946, #0e1c34);
      color: var(--text);
    }

    .gallery-list {
      display: flex;
      flex-direction: column;
      gap: 18px;
    }

    .gallery-row {
      display: grid;
      grid-template-columns: 280px 1fr;
      gap: 20px;
      align-items: center;
      background: linear-gradient(180deg, rgba(11,22,43,0.98), rgba(15,29,54,0.98));
      border: 1px solid rgba(56,189,248,0.14);
      border-radius: 20px;
      padding: 16px;
      box-shadow: var(--shadow);
    }

    .gallery-thumb {
      width: 100%;
      height: 190px;
      object-fit: cover;
      border-radius: 14px;
      background: #000;
      border: 1px solid rgba(56,189,248,0.08);
    }

    .gallery-meta {
      min-width: 0;
    }

    .gallery-name {
      color: var(--blue);
      font-size: 20px;
      font-weight: 800;
      margin-bottom: 12px;
      word-break: break-word;
    }

    .meta-line {
      color: var(--text);
      opacity: 0.95;
      margin: 8px 0;
      font-size: 15px;
    }

    .meta-label {
      color: var(--muted);
      display: inline-block;
      min-width: 86px;
    }

    .empty-state {
      padding: 28px;
      border-radius: 18px;
      background: rgba(15,29,54,0.72);
      border: 1px dashed rgba(56,189,248,0.18);
      color: var(--muted);
    }

    @media (max-width: 1080px) {
      .hero-grid {
        grid-template-columns: 1fr;
      }

      .gallery-row {
        grid-template-columns: 1fr;
      }

      .gallery-thumb {
        height: auto;
        max-height: 360px;
      }
    }

    @media (max-width: 900px) {
      .app-shell {
        flex-direction: column;
      }

      .sidebar {
        width: 100%;
        height: auto;
        position: relative;
        border-right: none;
        border-bottom: 1px solid rgba(56,189,248,0.12);
      }

      .main {
        padding: 24px 16px 32px;
      }

      .page-head {
        flex-direction: column;
        align-items: flex-start;
      }

      .page-title {
        font-size: 30px;
      }
    }
  </style>
</head>
<body>
  <div class="app-shell">
    <aside class="sidebar">
      <div class="brand">SafeStep</div>
      <div class="nav-title">Navigation</div>
      <div class="nav-links">
        <a class="${navDashboardClass}" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Dashboard</a>
        <a class="${navLatestClass}" href="/latest-image">Latest Capture</a>
        <a class="${navGalleryClass}" href="/gallery">Saved Images</a>
      </div>
    </aside>

    <main class="main">
      ${content}
    </main>
  </div>
</body>
</html>
  `;
}

function renderHomePage(req) {
  const current = latestCapture || loadLatestCaptureFromDisk(req);

  const content = `
    <div class="page-head">
      <div>
        <h1 class="page-title">SafeStep Night System</h1>
        <div class="page-subtitle">Caregiver Monitoring Dashboard</div>
      </div>
      <div class="status-pill">
        <span class="status-dot"></span>
        Backend Server Running
      </div>
    </div>

    <div class="section-label">Live Status</div>
    <div class="hero-grid">
      <div class="stat-card">
        <div class="stat-label">Server</div>
        <div class="stat-value">Online</div>
        <div class="stat-subtext">Image upload and gallery service active</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Latest Upload</div>
        <div class="stat-value">${current ? "Available" : "Waiting"}</div>
        <div class="stat-subtext">${current ? escapeHtml(formatDateTime(current.timestamp)) : "No image uploaded yet"}</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Saved Images</div>
        <div class="stat-value">${loadAllCaptures(req).length}</div>
        <div class="stat-subtext">Stored in backend uploads folder</div>
      </div>
    </div>

    <div class="section-label">Latest Image Captured</div>
    <div class="card">
      ${
        current
          ? `
            <div class="meta-line"><span class="meta-label">Filename</span> ${escapeHtml(current.filename)}</div>
            <div class="meta-line"><span class="meta-label">Captured</span> ${escapeHtml(formatDateTime(current.timestamp))}</div>

            <div class="image-wrap">
              <img src="${current.imageUrl}" alt="Latest SafeStep capture" />
            </div>

            <div class="button-row">
              <a class="btn btn-primary" href="/latest-image">Open Latest Capture</a>
              <a class="btn btn-outline" href="/gallery">Open Gallery</a>
              <a class="btn btn-outline" href="${current.fullUrl}" target="_blank" rel="noopener noreferrer">Open Raw Image</a>
              <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Go to Dashboard</a>
            </div>
          `
          : `
            <div class="empty-state">
              No image uploaded yet. Once the ESP32 sends a capture, it will appear here.
            </div>
            <div class="button-row">
              <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Go to Dashboard</a>
            </div>
          `
      }
    </div>
  `;

  return layoutPage({
    title: "SafeStep Backend",
    active: "dashboard",
    content,
  });
}

function renderLatestImagePage(req) {
  const current = latestCapture || loadLatestCaptureFromDisk(req);

  const content = `
    <div class="page-head">
      <div>
        <h1 class="page-title">Latest Image Captured</h1>
        <div class="page-subtitle">Most recent SafeStep upload from the backend server</div>
      </div>
      <div class="status-pill">
        <span class="status-dot"></span>
        ${current ? "Image Available" : "No Upload Yet"}
      </div>
    </div>

    <div class="section-label">Capture Preview</div>
    <div class="card">
      ${
        current
          ? `
            <div class="hero-grid" style="grid-template-columns: repeat(3, minmax(180px, 1fr));">
              <div class="stat-card">
                <div class="stat-label">Filename</div>
                <div class="stat-value" style="font-size:20px;">${escapeHtml(current.filename)}</div>
                <div class="stat-subtext">Latest stored image</div>
              </div>
              <div class="stat-card">
                <div class="stat-label">Captured At</div>
                <div class="stat-value" style="font-size:20px;">${escapeHtml(formatDateTime(current.timestamp))}</div>
                <div class="stat-subtext">Server file timestamp</div>
              </div>
              <div class="stat-card">
                <div class="stat-label">Source</div>
                <div class="stat-value" style="font-size:20px;">ESP32 Upload</div>
                <div class="stat-subtext">Received by /api/upload-raw</div>
              </div>
            </div>

            <div class="image-wrap">
              <img src="${current.imageUrl}" alt="Latest uploaded SafeStep image" />
            </div>

            <div class="button-row">
              <a class="btn btn-primary" href="${current.fullUrl}" target="_blank" rel="noopener noreferrer">Open Raw Image</a>
              <a class="btn btn-outline" href="/gallery">View All Saved Images</a>
              <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Back to Dashboard</a>
            </div>
          `
          : `
            <div class="empty-state">
              No image has been uploaded yet.
            </div>
            <div class="button-row">
              <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Back to Dashboard</a>
            </div>
          `
      }
    </div>
  `;

  return layoutPage({
    title: "Latest SafeStep Capture",
    active: "latest",
    content,
  });
}

function renderSingleImagePage(req, file) {
  const content = `
    <div class="page-head">
      <div>
        <h1 class="page-title">Saved Image</h1>
        <div class="page-subtitle">Single capture view</div>
      </div>
      <div class="status-pill">
        <span class="status-dot"></span>
        Stored Capture
      </div>
    </div>

    <div class="section-label">Preview</div>
    <div class="card">
      <div class="meta-line"><span class="meta-label">Filename</span> ${escapeHtml(file.filename)}</div>
      <div class="meta-line"><span class="meta-label">Captured</span> ${escapeHtml(formatDateTime(file.timestamp))}</div>

      <div class="image-wrap">
        <img src="${file.imageUrl}" alt="${escapeHtml(file.filename)}" />
      </div>

      <div class="button-row">
        <a class="btn btn-primary" href="${file.fullUrl}" target="_blank" rel="noopener noreferrer">Open Raw Image</a>
        <a class="btn btn-outline" href="/gallery">Back to Gallery</a>
        <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Back to Dashboard</a>
      </div>
    </div>
  `;

  return layoutPage({
    title: file.filename,
    active: "gallery",
    content,
  });
}

function renderGalleryPage(req) {
  const files = loadAllCaptures(req);

  const imagesHtml = files.length
    ? `
      <div class="gallery-list">
        ${files
          .map(
            (file) => `
              <div class="gallery-row">
                <div>
                  <img class="gallery-thumb" src="${file.imageUrl}" alt="${escapeHtml(file.filename)}" />
                </div>

                <div class="gallery-meta">
                  <div class="gallery-name">${escapeHtml(file.filename)}</div>
                  <div class="meta-line"><span class="meta-label">Captured</span> ${escapeHtml(formatDateTime(file.timestamp))}</div>
                  <div class="meta-line"><span class="meta-label">Raw URL</span> <a href="${file.fullUrl}" target="_blank" rel="noopener noreferrer">Open image file</a></div>

                  <div class="button-row">
                    <a class="btn btn-primary" href="/image/${encodeURIComponent(file.filename)}">View Image</a>
                    <a class="btn btn-outline" href="${file.fullUrl}" target="_blank" rel="noopener noreferrer">Open Raw</a>
                    <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Dashboard</a>
                  </div>
                </div>
              </div>
            `
          )
          .join("")}
      </div>
    `
    : `
      <div class="empty-state">No uploaded images yet.</div>
      <div class="button-row">
        <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Back to Dashboard</a>
      </div>
    `;

  const content = `
    <div class="page-head">
      <div>
        <h1 class="page-title">Saved Images</h1>
        <div class="page-subtitle">SafeStep image gallery with dashboard styling</div>
      </div>
      <div class="status-pill">
        <span class="status-dot"></span>
        ${files.length} Stored
      </div>
    </div>

    <div class="section-label">Gallery</div>
    ${imagesHtml}
  `;

  return layoutPage({
    title: "SafeStep Gallery",
    active: "gallery",
    content,
  });
}

async function sendCaregiverEscalationEmail({
  event = "fall",
  escalatedAt,
  imageUrl = "",
  dashboardUrl = "",
  latestPageUrl = "",
}) {
  const subject = `SafeStep Alert Escalation: ${String(event).toUpperCase()} not acknowledged`;

  const resolvedDashboardUrl = dashboardUrl || CAREGIVER_DASHBOARD_URL;

  const text = [
    "SafeStep Night System Alert Escalation",
    "",
    `Event: ${event}`,
    `Escalated At: ${escalatedAt}`,
    "Acknowledgement: NOT RECEIVED before timeout",
    resolvedDashboardUrl ? `ESP32 Dashboard: ${resolvedDashboardUrl}` : "",
    latestPageUrl ? `Latest Capture Page: ${latestPageUrl}` : "",
    imageUrl ? `Latest Raw Image: ${imageUrl}` : "",
    "",
    "Caregiver action is required.",
  ]
    .filter(Boolean)
    .join("\n");

  const html = `
    <div style="font-family:Arial,sans-serif;background:#020817;color:#e2e8f0;padding:24px;">
      <div style="max-width:700px;margin:0 auto;background:#0b162b;border:1px solid #183153;border-radius:18px;padding:24px;">
        <h2 style="margin-top:0;color:#fb7185;">SafeStep Alert Escalation</h2>
        <p>A fall event was detected and was <strong>not acknowledged</strong> before the escalation timeout.</p>

        <table style="width:100%;border-collapse:collapse;margin:16px 0;">
          <tr>
            <td style="padding:10px;border-bottom:1px solid #183153;"><strong>Event</strong></td>
            <td style="padding:10px;border-bottom:1px solid #183153;">${escapeHtml(event)}</td>
          </tr>
          <tr>
            <td style="padding:10px;border-bottom:1px solid #183153;"><strong>Escalated At</strong></td>
            <td style="padding:10px;border-bottom:1px solid #183153;">${escapeHtml(escalatedAt)}</td>
          </tr>
          ${
            resolvedDashboardUrl
              ? `<tr>
                  <td style="padding:10px;border-bottom:1px solid #183153;"><strong>ESP32 Dashboard</strong></td>
                  <td style="padding:10px;border-bottom:1px solid #183153;">
                    <a href="${resolvedDashboardUrl}" style="color:#38bdf8;">Open dashboard</a>
                  </td>
                </tr>`
              : ""
          }
          ${
            latestPageUrl
              ? `<tr>
                  <td style="padding:10px;border-bottom:1px solid #183153;"><strong>Latest Capture Page</strong></td>
                  <td style="padding:10px;border-bottom:1px solid #183153;">
                    <a href="${latestPageUrl}" style="color:#38bdf8;">Open latest capture</a>
                  </td>
                </tr>`
              : ""
          }

        </table>

        <p style="color:#94a3b8;">This notification was generated automatically by the SafeStep backend server.</p>
      </div>
    </div>
  `;

  return transporter.sendMail({
    from: EMAIL_FROM,
    to: CAREGIVER_EMAIL,
    subject,
    text,
    html,
  });
}

app.get("/", (req, res) => {
  return res.send(renderHomePage(req));
});

app.get("/latest-image", (req, res) => {
  return res.send(renderLatestImagePage(req));
});

app.get("/gallery", (req, res) => {
  return res.send(renderGalleryPage(req));
});

app.get("/image/:filename", (req, res) => {
  const filename = path.basename(req.params.filename || "");
  const filepath = path.join(uploadDir, filename);

  if (!fs.existsSync(filepath)) {
    return res.status(404).send(
      layoutPage({
        title: "Image Not Found",
        active: "gallery",
        content: `
          <div class="page-head">
            <div>
              <h1 class="page-title">Image Not Found</h1>
              <div class="page-subtitle">The requested capture does not exist.</div>
            </div>
          </div>
          <div class="card">
            <div class="empty-state">
              The requested image could not be found.
            </div>
            <div class="button-row">
              <a class="btn btn-outline" href="/gallery">Back to Gallery</a>
              <a class="btn btn-outline" href="${CAREGIVER_DASHBOARD_URL}" target="_self">Back to Dashboard</a>
            </div>
          </div>
        `,
      })
    );
  }

  const stat = fs.statSync(filepath);
  const baseUrl = getBaseUrl(req);

  const file = {
    filename,
    timestamp: stat.mtime.toISOString(),
    imageUrl: `/uploads/${filename}`,
    fullUrl: `${baseUrl}/uploads/${filename}`,
  };

  return res.send(renderSingleImagePage(req, file));
});

app.post("/api/upload-raw", (req, res) => {
  try {
    console.log("POST /api/upload-raw received");

    if (!req.body || req.body.length === 0) {
      console.log("No image data received");
      return res.status(400).json({
        success: false,
        message: "No image data received",
      });
    }

    console.log(`Image bytes received: ${req.body.length}`);

    const filename = `capture_${Date.now()}.jpg`;
    const filepath = path.join(uploadDir, filename);

    fs.writeFileSync(filepath, req.body);

    const baseUrl = getBaseUrl(req);

    latestCapture = {
      filename,
      imageUrl: `/uploads/${filename}`,
      fullUrl: `${baseUrl}/uploads/${filename}`,
      latestPageUrl: `${baseUrl}/latest-image`,
      timestamp: new Date().toISOString(),
    };

    console.log("Saved image:", filepath);

    return res.json({
      success: true,
      message: "Image uploaded successfully",
      data: latestCapture,
    });
  } catch (err) {
    console.error("Upload error:", err);
    return res.status(500).json({
      success: false,
      message: "Server error",
    });
  }
});

app.get("/api/latest", (req, res) => {
  const current = latestCapture || loadLatestCaptureFromDisk(req);

  if (!current) {
    return res.status(404).json({
      success: false,
      message: "No image uploaded yet",
    });
  }

  latestCapture = current;

  return res.json({
    success: true,
    data: current,
  });
});

app.post("/api/escalate", async (req, res) => {
  try {
    console.log("POST /api/escalate received", req.body);

    const event = String(req.body?.event || "fall").toLowerCase();
    const source = String(req.body?.source || "esp32");
    const escalatedAt = new Date().toISOString();

    const dashboardUrl = String(req.body?.dashboardUrl || "").trim() || CAREGIVER_DASHBOARD_URL;

    const currentImage = latestCapture || loadLatestCaptureFromDisk(req);
    const imageUrl = currentImage?.fullUrl || "";
    const latestPageUrl = `${getBaseUrl(req)}/latest-image`;

    latestEscalation = {
      event,
      source,
      escalatedAt,
      acknowledged: false,
      emailTarget: CAREGIVER_EMAIL,
      dashboardUrl,
      imageUrl,
      latestPageUrl,
      emailSent: false,
      emailError: null,
    };

    try {
      const info = await sendCaregiverEscalationEmail({
        event,
        escalatedAt,
        imageUrl,
        dashboardUrl,
        latestPageUrl,
      });

      latestEscalation.emailSent = true;
      latestEscalation.emailMessageId = info.messageId || null;

      console.log("Escalation email sent:", info.messageId);

      return res.json({
        success: true,
        message: "Escalation email sent",
        data: latestEscalation,
      });
    } catch (emailErr) {
      latestEscalation.emailSent = false;
      latestEscalation.emailError = String(emailErr?.message || emailErr);

      console.error("Escalation email failed:", emailErr);

      return res.status(500).json({
        success: false,
        message: "Escalation received but email failed",
        data: latestEscalation,
      });
    }
  } catch (err) {
    console.error("Escalation server error:", err);
    return res.status(500).json({
      success: false,
      message: "Server error",
    });
  }
});

app.get("/api/escalations/latest", (req, res) => {
  if (!latestEscalation) {
    return res.status(404).json({
      success: false,
      message: "No escalation yet",
    });
  }

  return res.json({
    success: true,
    data: latestEscalation,
  });
});

app.listen(PORT, "0.0.0.0", () => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
});
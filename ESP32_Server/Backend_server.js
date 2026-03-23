const express = require("express");
const cors = require("cors");
const fs = require("fs");
const path = require("path");

const app = express();
const PORT = 5000;

app.use(cors());
app.use("/uploads", express.static(path.join(__dirname, "uploads")));
app.use(express.raw({ type: "application/octet-stream", limit: "10mb" }));

const uploadDir = path.join(__dirname, "uploads");
if (!fs.existsSync(uploadDir)) {
  fs.mkdirSync(uploadDir);
}

let latestCapture = null;

function getBaseUrl(req) {
  return `${req.protocol}://${req.get("host")}`;
}

function loadLatestCaptureFromDisk(req) {
  const files = fs
    .readdirSync(uploadDir)
    .filter((f) => f.toLowerCase().endsWith(".jpg"))
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
    timestamp: latest.timestamp,
  };
}

app.get("/", (req, res) => {
  const current = latestCapture || loadLatestCaptureFromDisk(req);

  if (!current) {
    return res.send(`
      <h2>Backend is running</h2>
      <p>No image uploaded yet.</p>
      <p><a href="/gallery">Open gallery</a></p>
    `);
  }

  return res.send(`
    <h2>Latest Capture</h2>
    <p>Filename: ${current.filename}</p>
    <p>Time: ${current.timestamp}</p>
    <p><a href="/gallery">Open gallery</a></p>
    <img src="${current.fullUrl}" width="500" />
  `);
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

app.get("/gallery", (req, res) => {
  const baseUrl = getBaseUrl(req);

  const dashboardBackUrl =
    typeof req.query.back === "string" && req.query.back.trim()
      ? req.query.back.trim()
      : "/";

  const files = fs
    .readdirSync(uploadDir)
    .filter((f) => f.toLowerCase().endsWith(".jpg"))
    .map((filename) => {
      const filepath = path.join(uploadDir, filename);
      const stat = fs.statSync(filepath);
      return {
        filename,
        url: `${baseUrl}/uploads/${filename}`,
        timestamp: stat.mtime.toISOString(),
        mtimeMs: stat.mtimeMs,
      };
    })
    .sort((a, b) => b.mtimeMs - a.mtimeMs);

  const cards =
    files.length === 0
      ? `<p style="color:#94a3b8;">No saved images yet.</p>`
      : files
          .map(
            (img) => `
              <div class="card">
                <a href="${img.url}" target="_blank">
                  <img src="${img.url}" alt="${img.filename}" />
                </a>
                <div class="meta">
                  <div class="name">${img.filename}</div>
                  <div class="time">${img.timestamp}</div>
                </div>
              </div>
            `
          )
          .join("");

  res.send(`
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="utf-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1" />
      <title>SafeStep Saved Images</title>
      <style>
        body {
          margin: 0;
          font-family: Arial, sans-serif;
          background: #0a0e17;
          color: #e2e8f0;
          padding: 24px;
        }
        h1 {
          margin-bottom: 8px;
          color: #38bdf8;
        }
        p.sub {
          color: #94a3b8;
          margin-bottom: 24px;
        }
        .topbar {
          display:flex;
          justify-content:space-between;
          align-items:center;
          flex-wrap:wrap;
          gap:12px;
          margin-bottom:20px;
        }
        .btn {
          display:inline-block;
          text-decoration:none;
          background:#38bdf8;
          color:#0a0e17;
          padding:10px 14px;
          border-radius:8px;
          font-weight:bold;
        }
        .grid {
          display:grid;
          grid-template-columns:repeat(auto-fill, minmax(240px, 1fr));
          gap:16px;
        }
        .card {
          background:#111827;
          border:1px solid #1e2d40;
          border-radius:14px;
          overflow:hidden;
        }
        .card img {
          width:100%;
          height:200px;
          object-fit:cover;
          display:block;
          background:#000;
        }
        .meta {
          padding:12px;
        }
        .name {
          font-size:.95rem;
          font-weight:bold;
          margin-bottom:6px;
          word-break:break-word;
        }
        .time {
          color:#94a3b8;
          font-size:.8rem;
        }
      </style>
    </head>
    <body>
      <div class="topbar">
        <div>
          <h1>Saved Captures</h1>
          <p class="sub">All uploaded images from the ESP32-CAM</p>
        </div>
        <a class="btn" href="${dashboardBackUrl}">Back to Dashboard</a>
      </div>
      <div class="grid">
        ${cards}
      </div>
    </body>
    </html>
  `);
});

app.listen(PORT, "0.0.0.0", () => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
});
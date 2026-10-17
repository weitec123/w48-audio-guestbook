#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Audio Guestbook W48</title>
  <style>
    * { box-sizing: border-box; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: #f4f6f9; color: #333; margin: 0; padding: 20px; }
    .container { max-width: 800px; margin: 0 auto; }
    h1 { color: #1a1a2e; margin-bottom: 5px; }
    .subtitle { color: #666; margin-bottom: 25px; }
    .card { background: white; border-radius: 12px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 12px rgba(0,0,0,0.06); }
    .card h2 { margin-top: 0; font-size: 1.25rem; border-bottom: 2px solid #eef0f4; padding-bottom: 10px; color: #2c3e50; }
    .btn { background: #007bff; color: white; border: none; padding: 10px 18px; border-radius: 6px; font-weight: 600; cursor: pointer; text-decoration: none; display: inline-block; font-size: 14px; }
    .btn:hover { background: #0056b3; }
    .btn-danger { background: #dc3545; }
    .btn-danger:hover { background: #a71d2a; }
    .btn-success { background: #28a745; }
    .btn-success:hover { background: #1e7e34; }
    .btn-sm { padding: 5px 10px; font-size: 12px; }
    .slider-box { display: flex; align-items: center; gap: 15px; margin: 15px 0; }
    input[type=range] { flex: 1; accent-color: #007bff; }
    table { width: 100%; border-collapse: collapse; margin-top: 15px; }
    th, td { text-align: left; padding: 10px; border-bottom: 1px solid #eef0f4; }
    th { background: #fafbfc; color: #555; }
    .empty-state { text-align: center; color: #888; padding: 30px 0; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: 500; font-size: 14px; }
    input[type=text], input[type=password], input[type=number] { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 6px; font-size: 14px; }
    .badge { background: #e3f2fd; color: #0d47a1; padding: 4px 8px; border-radius: 4px; font-size: 12px; font-weight: bold; }
    .badge-pill { border-radius: 50px; padding: 2px 10px; }
    .progress-bar { background: #e9ecef; border-radius: 8px; height: 16px; overflow: hidden; margin: 10px 0; }
    .progress-fill { background: #28a745; height: 100%; width: %USAGE_PERCENT%%; transition: width 0.3s; }
    .switch-label { display: flex; align-items: center; gap: 10px; cursor: pointer; font-size: 15px; font-weight: 600; }
    .switch-label input { width: 20px; height: 20px; }
    .grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; }
  </style>
</head>
<body>
<div class="container">
  <h1>☎️ Audio Guestbook W48</h1>
  <div class="subtitle">Event-Verwaltung &amp; Konfiguration</div>

  <!-- Speicherplatz Info -->
  <div class="card">
    <h2>💾 Speicherplatz</h2>
    <div style="display:flex; justify-content:space-between; font-weight:600; font-size:14px;">
      <span>%FREE_SPACE% frei von %TOTAL_SPACE%</span>
      <span class="badge">%REMAINING_TIME%</span>
    </div>
    <div class="progress-bar">
      <div class="progress-fill"></div>
    </div>
    <small style="color: #666;">Aktueller Speicher: Interner Flash (LittleFS).</small>
  </div>

  <!-- Lautstaerke -->
  <div class="card">
    <h2>🔊 H&ouml;rer-Lautst&auml;rke</h2>
    <div class="slider-box">
      <input type="range" id="volRange" min="10" max="100" value="%VOLUME%" oninput="updateVolLabel(this.value)" onchange="saveVolume(this.value)">
      <span id="volVal" style="font-weight:bold; min-width: 45px;">%VOLUME% %</span>
    </div>
    <small style="color: #666;">Wiedergabelautst&auml;rke am H&ouml;rer.</small>
  </div>

  <!-- Mikrofon-Empfindlichkeit -->
  <div class="card">
    <h2>🎤 Mikrofon-Aufnahmelautst&auml;rke</h2>
    <div class="slider-box">
      <input type="range" id="micRange" min="2" max="25" value="%MIC_GAIN%" oninput="updateMicLabel(this.value)" onchange="saveMicGain(this.value)">
      <span id="micVal" style="font-weight:bold; min-width: 45px;">%MIC_GAIN% x</span>
    </div>
    <small style="color: #666;">Empfindlichkeit des Mikrofons (niedriger = weniger Hintergrundrauschen, h&ouml;her = lauter).</small>
  </div>

  <!-- Begruessung -->
  <div class="card">
    <h2>🎙️ Begr&uuml;&szlig;ungsansage</h2>
    <p>Aktuelle Ansage: <span class="badge">%GREETING_STATUS%</span></p>
    <div style="display:flex; gap:10px; align-items:center; flex-wrap: wrap;">
      <button class="btn btn-success" onclick="playTest()">▶️ Ansage am H&ouml;rer abspielen</button>
      <form action="/upload_greeting" method="POST" enctype="multipart/form-data" style="display:inline-flex; gap:10px; align-items:center;">
        <input type="file" name="greeting" accept=".wav" required>
        <button type="submit" class="btn">Neue .wav hochladen</button>
      </form>
    </div>
  </div>

  <!-- Aufnahmen -->
  <div class="card">
    <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px;">
      <h2 style="margin-bottom:0; border-bottom:none; padding-bottom:0;">📂 Aufgenommene Nachrichten (<span id="recCount">%REC_COUNT%</span>)</h2>
      <div style="display:flex; gap: 8px;">
        <a href="/download_all" class="btn btn-success">📦 Alle als ZIP herunterladen</a>
        <button class="btn btn-danger" onclick="clearAll()">🗑️ Event zur&uuml;cksetzen</button>
      </div>
    </div>
    <div id="recordingsList">%RECORDINGS_TABLE%</div>
  </div>

  <!-- Waehlscheiben-Codes -->
  <div class="card">
    <h2>🔢 W&auml;hlscheiben-Codes &amp; Funktionen</h2>
    <div style="background:#eef6ff; border-left:4px solid #3498db; padding:10px 14px; margin-bottom:15px; border-radius:4px; font-size:13px; line-height:1.4;">
      <strong>💡 Toleranz-Raster f&uuml;r historische W48-W&auml;hlscheiben:</strong><br>
      Um Fehlw&auml;hlungen durch gealterte Mechanik (Prellen, Toleranzen) v&ouml;llig auszuschlie&szlig;en, sind <strong>nur die Ziffern 1, 5 und 9</strong> zul&auml;ssig:<br>
      &bull; <strong>1</strong> (1&ndash;3 Impulse) &nbsp;|&nbsp; &bull; <strong>5</strong> (4&ndash;6 Impulse) &nbsp;|&nbsp; &bull; <strong>9</strong> (7&ndash;11 Impulse)<br>
      Damit funktionieren auch mehrstellige PINs wie <code>199</code>, <code>599</code>, <code>999</code> oder <code>9999</code> zu 100% narrensicher.
    </div>
    <form action="/save_codes" method="POST">
      <table>
        <thead>
          <tr><th>Funktion</th><th>Code (nur 1, 5, 9)</th><th>Status / Datei</th><th>Aktion</th></tr>
        </thead>
        <tbody>
          <tr>
            <td><strong>Letzte Aufnahme anh&ouml;ren</strong></td>
            <td><input type="text" name="code_last" value="%CODE_LAST%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td><span class="badge">System</span></td>
            <td><em>Am H&ouml;rer w&auml;hlen</em></td>
          </tr>
          <tr>
            <td><strong>Easter Egg 1 anh&ouml;ren (Witz)</strong></td>
            <td><input type="text" name="code_e1" value="%CODE_E1%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td>%EASTER1_STATUS%</td>
            <td><em>Am H&ouml;rer w&auml;hlen</em></td>
          </tr>
          <tr>
            <td><strong>Easter Egg 2 anh&ouml;ren (St&auml;ndchen)</strong></td>
            <td><input type="text" name="code_e2" value="%CODE_E2%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td>%EASTER7_STATUS%</td>
            <td><em>Am H&ouml;rer w&auml;hlen</em></td>
          </tr>
          <tr>
            <td><strong>Neue Ansage aufsprechen (Admin PIN)</strong></td>
            <td><input type="text" name="code_admin" value="%CODE_ADMIN%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td><span class="badge">Aufnahme</span></td>
            <td><em>Am H&ouml;rer w&auml;hlen &amp; sprechen</em></td>
          </tr>
          <tr>
            <td><strong>Easter Egg 1 NEU aufnehmen (Witz)</strong></td>
            <td><input type="text" name="code_rec_e1" value="%CODE_REC_E1%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td><span class="badge">Aufnahme</span></td>
            <td><em>W&auml;hlen, nach Beep sprechen</em></td>
          </tr>
          <tr>
            <td><strong>Easter Egg 2 NEU aufnehmen (St&auml;ndchen)</strong></td>
            <td><input type="text" name="code_rec_e2" value="%CODE_REC_E2%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td><span class="badge">Aufnahme</span></td>
            <td><em>W&auml;hlen, nach Beep sprechen</em></td>
          </tr>
          <tr>
            <td><strong>Zufalls-Bimmeln Umschalten (An/Aus)</strong></td>
            <td><input type="text" name="code_ring" value="%CODE_RING%" pattern="[159]*" maxlength="6" oninput="this.value = this.value.replace(/[^159]/g, '')" title="Nur 1, 5 oder 9" style="width:70px; text-align:center; font-weight:bold;"></td>
            <td><span class="badge">Quittungston</span></td>
            <td><em>Am H&ouml;rer w&auml;hlen</em></td>
          </tr>
        </tbody>
      </table>
      <div style="margin-top:12px; display:flex; justify-content:space-between; align-items:center;">
        <button type="submit" class="btn">💾 Codes speichern</button>
        <span class="hint">Ausschlie&szlig;lich Kombinationen aus 1, 5 und 9 erlaubt (z. B. 1, 5, 9, 199, 599, 999, 9999).</span>
      </div>
    </form>

    <div style="margin-top:15px; padding-top:12px; border-top:1px solid #eee; display:flex; flex-direction:column; gap:10px;">
      <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;">
        <div style="display:inline-flex; align-items:center; gap:8px;">
          <span style="font-size:12px; font-weight:bold;">Witz (Easter 1):</span>
          <button class="btn btn-sm" onclick="playTestAudio('e1')">▶️ Am H&ouml;rer abspielen</button>
        </div>
        <form action="/upload_easter1" method="POST" enctype="multipart/form-data" style="display:inline-flex; align-items:center; gap:8px;">
          <input type="file" name="e1" accept=".wav" required style="width:160px; font-size:12px;">
          <button type="submit" class="btn btn-sm">Upload</button>
        </form>
        <form action="/reset_audio" method="POST" style="display:inline;">
          <input type="hidden" name="type" value="easter1">
          <button type="submit" class="btn btn-sm btn-danger" onclick="return confirm('Witz wirklich auf Standard zurücksetzen?')">Standard wiederherstellen</button>
        </form>
      </div>

      <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;">
        <div style="display:inline-flex; align-items:center; gap:8px;">
          <span style="font-size:12px; font-weight:bold;">St&auml;ndchen (Easter 2):</span>
          <button class="btn btn-sm" onclick="playTestAudio('e2')">▶️ Am H&ouml;rer abspielen</button>
        </div>
        <form action="/upload_easter7" method="POST" enctype="multipart/form-data" style="display:inline-flex; align-items:center; gap:8px;">
          <input type="file" name="e7" accept=".wav" required style="width:160px; font-size:12px;">
          <button type="submit" class="btn btn-sm">Upload</button>
        </form>
        <form action="/reset_audio" method="POST" style="display:inline;">
          <input type="hidden" name="type" value="easter7">
          <button type="submit" class="btn btn-sm btn-danger" onclick="return confirm('Ständchen wirklich auf Standard zurücksetzen?')">Standard wiederherstellen</button>
        </form>
      </div>
    </div>
  </div>

  <!-- Event-Schutz & Zufalls-Klingeln -->
  <div class="card">
    <h2>🛎️ Event-Optionen &amp; Bimmeln</h2>
    <form action="/save_event_options" method="POST">
      <div class="grid-2">
        <div class="form-group">
          <label>Minimale Aufnahmedauer (Sekunden):</label>
          <input type="number" name="min_sec" value="%MIN_SEC%" min="1" max="10">
          <small style="color:#666;">K&uuml;rzere Versuche werden automatisch verworfen.</small>
        </div>
        <div class="form-group">
          <label>Maximale Aufnahmedauer (Sekunden):</label>
          <input type="number" name="max_sec" value="%MAX_SEC%" min="30" max="600">
          <small style="color:#666;">Stoppt automatisch mit Besetztzeichen.</small>
        </div>
      </div>

      <hr style="border:0; border-top:1px solid #eee; margin:15px 0;">

      <div class="form-group">
        <label class="switch-label">
          <input type="checkbox" name="ring_active" value="1" %RING_CHECKED%>
          🔔 Zuf&auml;lliges Bimmeln aktivieren (G&auml;ste anlocken) &ndash; <em>auch per Code 99 schaltbar</em>
        </label>
        <div style="margin-top:10px;" class="grid-2">
          <div>
            <label>Alle wie viele Minuten klingeln?</label>
            <input type="number" name="ring_interval" value="%RING_INTERVAL%" min="5" max="120">
          </div>
        </div>
      </div>
      <button type="submit" class="btn">Optionen speichern</button>
    </form>
  </div>

  <!-- Einstellungen -->
  <div class="card">
    <h2>⚙️ Einstellungen (WLAN &amp; Server)</h2>
    <form action="/save_settings" method="POST">
      <div class="form-group">
        <label>Heim-WLAN (SSID):</label>
        <input type="text" name="ssid" value="%SSID%">
      </div>
      <div class="form-group">
        <label>WLAN Passwort:</label>
        <input type="password" name="pass" value="%PASS%">
      </div>
      <div class="form-group">
        <label>Foto4box API / Webhook URL (optional):</label>
        <input type="text" name="webhook" value="%WEBHOOK%" placeholder="https://api.foto4box.de/upload/...">
      </div>
      <button type="submit" class="btn">Speichern &amp; Verbinden</button>
    </form>
  </div>
</div>

<script>
function updateVolLabel(val) {
  document.getElementById('volVal').innerText = val + ' %';
}
function saveVolume(val) {
  fetch('/set_volume?vol=' + val);
}
function updateMicLabel(val) {
  document.getElementById('micVal').innerText = val + ' x';
}
function saveMicGain(val) {
  fetch('/set_mic_gain?gain=' + val);
}
function playTest() {
  fetch('/play_test');
}
function playTestAudio(target) {
  fetch('/play_test?target=' + target);
}
function clearAll() {
  if (confirm("Wirklich ALLE Aufnahmen unwiderruflich loeschen fuer die naechste Feier?")) {
    fetch('/clear_all', {method: 'POST'}).then(() => location.reload());
  }
}
function deleteFile(name) {
  if (confirm("Datei " + name + " loeschen?")) {
    fetch('/delete_file?file=' + encodeURIComponent(name), {method: 'POST'}).then(() => location.reload());
  }
}
</script>
</body>
</html>
)rawliteral";

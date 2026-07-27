const $ = (id) => document.getElementById(id);
let busy = false;
let lastResultLoaded = "";

function toast(message) {
  const el = $("toast");
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { el.hidden = true; }, 4200);
}

async function api(url, options = {}) {
  const response = await fetch(url, { cache: "no-store", ...options });
  const text = await response.text();
  let data;
  try { data = JSON.parse(text); } catch { data = text; }
  if (!response.ok) {
    throw new Error(data?.error || text || `HTTP ${response.status}`);
  }
  return data;
}

function setBusy(value) {
  busy = value;
  $("captureButton").disabled = value;
  $("testButton").disabled = value || !$("referenceSelect").value;
  $("referenceSelect").disabled = value;
  $("referenceName").disabled = value;
}

function formatSeconds(ms) {
  return `${(ms / 1000).toFixed(1).replace('.', ',')} с`;
}

function updateStatus(status) {
  const active = ["preparing", "scanning", "saving"].includes(status.state);
  setBusy(active);
  $("progressBar").style.width = `${status.progress || 0}%`;
  $("progressText").textContent = `${status.currentSource || 0} / ${status.totalSources || 1024}`;
  $("elapsedText").textContent = formatSeconds(status.elapsedMs || 0);
  $("statusMessage").textContent = status.message || "";

  const badge = $("stateBadge");
  badge.textContent = ({
    idle: "Ожидание", preparing: "Подготовка", scanning: "Измерение",
    saving: "Сохранение", completed: "Завершено", failed: "Ошибка"
  })[status.state] || status.state;
  badge.className = `badge ${status.state === 'completed' ? 'ok' : status.state === 'failed' ? 'error' : active ? 'busy' : 'neutral'}`;

  const s = status.statistics || {};
  for (const key of ["expectedLinks", "measuredLinks", "missingLinks", "extraLinks", "asymmetricLinks", "stuckLowPins"]) {
    $(key).textContent = s[key] ?? 0;
  }

  if (status.lastResult && status.state === "completed" && status.lastResult !== lastResultLoaded) {
    loadResult(status.lastResult);
    loadFiles();
  }
}

async function pollStatus() {
  try {
    updateStatus(await api("/api/status"));
  } catch (error) {
    $("statusMessage").textContent = `Нет связи с устройством: ${error.message}`;
  } finally {
    setTimeout(pollStatus, busy ? 350 : 1200);
  }
}

async function loadDevice() {
  try {
    const device = await api("/api/device");
    const badge = $("deviceBadge");
    badge.textContent = `${device.hardware} · ${device.ip} · ${device.link ? 'LINK' : 'NO LINK'}`;
    badge.className = `badge ${device.link ? 'ok' : 'error'}`;
  } catch (error) {
    $("deviceBadge").textContent = "Нет связи";
    $("deviceBadge").className = "badge error";
  }
}

function fileSize(bytes) {
  if (bytes < 1024) return `${bytes} Б`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} КиБ`;
  return `${(bytes / 1024 / 1024).toFixed(1)} МиБ`;
}

function renderFiles(element, files, type) {
  element.innerHTML = "";
  if (!files.length) {
    element.innerHTML = '<div class="empty">Файлов нет.</div>';
    return;
  }
  for (const item of files) {
    const row = document.createElement("div");
    row.className = "file-item";
    const info = document.createElement("div");
    info.innerHTML = `<strong>${item.file}</strong><br><small>${fileSize(item.size)}</small>`;
    const actions = document.createElement("div");
    actions.className = "file-actions";
    if (type === "result") {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = () => loadResult(item.file);
      actions.append(open);
    }
    const link = document.createElement("a");
    link.textContent = "Скачать";
    link.href = `/api/download?type=${encodeURIComponent(type)}&file=${encodeURIComponent(item.file)}`;
    actions.append(link);
    row.append(info, actions);
    element.append(row);
  }
}

async function loadFiles() {
  try {
    const [references, results] = await Promise.all([
      api("/api/references"), api("/api/results")
    ]);

    const select = $("referenceSelect");
    const selected = select.value;
    select.innerHTML = "";
    for (const item of references) {
      const option = document.createElement("option");
      option.value = item.file;
      option.textContent = item.file;
      select.append(option);
    }
    if (references.some((item) => item.file === selected)) select.value = selected;
    $("testButton").disabled = busy || !select.value;

    renderFiles($("referenceFiles"), references, "reference");
    renderFiles($("resultFiles"), results, "result");
  } catch (error) {
    toast(error.message);
  }
}

function renderPairs(target, pairs, truncated) {
  if (!pairs?.length) {
    target.innerHTML = '<div class="empty">Нет.</div>';
    return;
  }
  const rows = pairs.map((pair) => `<tr><td>${pair.a}</td><td>${pair.aName}</td><td>${pair.b}</td><td>${pair.bName}</td></tr>`).join("");
  target.innerHTML = `<div class="table-wrap"><table><thead><tr><th>A</th><th>Контакт A</th><th>B</th><th>Контакт B</th></tr></thead><tbody>${rows}</tbody></table></div>${truncated ? '<p>Список сокращён. Полное число указано в сводке.</p>' : ''}`;
}

async function loadResult(fileName) {
  try {
    const result = await api(`/api/result?file=${encodeURIComponent(fileName)}`);
    lastResultLoaded = fileName;
    const summary = result.summary || {};
    $("resultSummary").className = result.passed ? "result-ok" : "result-error";
    $("resultSummary").innerHTML = `
      <strong>${result.passed ? 'КАБЕЛЬ ИСПРАВЕН' : 'ОБНАРУЖЕНЫ ОТЛИЧИЯ'}</strong><br>
      Эталон: ${result.reference}<br>
      Обрывы: ${summary.missingLinks || 0}; паразитные связи: ${summary.extraLinks || 0};
      асимметрии: ${summary.asymmetricLinks || 0}; время: ${formatSeconds(result.elapsedMs || 0)}.
    `;
    $("errorTables").hidden = false;
    renderPairs($("missingTable"), result.missing, result.missingTruncated);
    renderPairs($("extraTable"), result.extra, result.extraTruncated);
    renderPairs($("asymmetricTable"), result.asymmetric, result.asymmetricTruncated);
  } catch (error) {
    toast(error.message);
  }
}

$("captureButton").addEventListener("click", async () => {
  const name = $("referenceName").value.trim();
  if (!name) return toast("Введите название эталона.");
  try {
    await api(`/api/reference/capture?name=${encodeURIComponent(name)}`, { method: "POST" });
    setBusy(true);
    toast("Эталонный замер запущен.");
  } catch (error) { toast(error.message); }
});

$("testButton").addEventListener("click", async () => {
  const reference = $("referenceSelect").value;
  if (!reference) return toast("Выберите эталон.");
  try {
    await api(`/api/test/start?reference=${encodeURIComponent(reference)}`, { method: "POST" });
    setBusy(true);
    toast("Проверка кабеля запущена.");
  } catch (error) { toast(error.message); }
});

$("refreshFilesButton").addEventListener("click", loadFiles);
$("refreshResultsButton").addEventListener("click", async () => {
  const files = await api("/api/results");
  if (files.length) loadResult(files[files.length - 1].file);
});

loadDevice();
loadFiles();
pollStatus();
setInterval(loadDevice, 10000);

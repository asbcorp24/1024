const $ = (id) => document.getElementById(id);
let busy = false;
let lastResultLoaded = "";
let eventSource;

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
  if (!response.ok) throw new Error(data?.error || text || `HTTP ${response.status}`);
  return data;
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
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
  const active = ["preparing", "scanning", "analyzing", "saving"].includes(status.state);
  setBusy(active);
  $("progressBar").style.width = `${status.progress || 0}%`;
  $("progressText").textContent = `${status.currentSource || 0} / ${status.totalSources || 1024}`;
  $("elapsedText").textContent = formatSeconds(status.elapsedMs || 0);
  $("statusMessage").textContent = status.message || "";

  const badge = $("stateBadge");
  badge.textContent = ({
    idle: "Ожидание", preparing: "Подготовка", scanning: "Измерение",
    analyzing: "Расчёт", saving: "Сохранение", completed: "Завершено", failed: "Ошибка"
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

async function loadInitialStatus() {
  try { updateStatus(await api("/api/status")); }
  catch (error) { $("statusMessage").textContent = `Нет связи с устройством: ${error.message}`; }
}

function connectEvents() {
  if (eventSource) eventSource.close();
  eventSource = new EventSource("/api/events");
  eventSource.addEventListener("status", (event) => {
    try { updateStatus(JSON.parse(event.data)); }
    catch (error) { console.error("Некорректное SSE-состояние", error); }
  });
  eventSource.addEventListener("files", () => loadFiles());
  eventSource.onopen = () => document.body.classList.remove("offline");
  eventSource.onerror = () => {
    document.body.classList.add("offline");
    $("statusMessage").textContent = "SSE-соединение восстанавливается…";
  };
}

async function loadDevice() {
  try {
    const device = await api("/api/device");
    const badge = $("deviceBadge");
    badge.textContent = `${device.hardware} · ${device.ip} · ${device.link ? 'LINK' : 'NO LINK'} · ASYNC`;
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
    const name = document.createElement("strong");
    name.textContent = item.file;
    const details = document.createElement("small");
    details.textContent = `${fileSize(item.size)}${item.kind ? ` · ${item.kind}` : ""}`;
    info.append(name, document.createElement("br"), details);

    const actions = document.createElement("div");
    actions.className = "file-actions";
    if (type === "calculation" && item.file.endsWith(".json")) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = () => loadResult(item.file);
      actions.append(open);
    }

    const link = document.createElement("a");
    link.textContent = "Выгрузить";
    link.href = `/api/download?type=${encodeURIComponent(type)}&file=${encodeURIComponent(item.file)}`;
    actions.append(link);
    row.append(info, actions);
    element.append(row);
  }
}

async function loadFiles() {
  try {
    const [references, calculations] = await Promise.all([
      api("/api/references"), api("/api/calculations")
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
    renderFiles($("calculationFiles"), calculations, "calculation");
  } catch (error) {
    toast(error.message);
  }
}

function renderPairs(target, pairs, truncated) {
  if (!pairs?.length) {
    target.innerHTML = '<div class="empty">Нет.</div>';
    return;
  }
  const rows = pairs.map((pair) => `<tr><td>${escapeHtml(pair.a)}</td><td>${escapeHtml(pair.aName)}</td><td>${escapeHtml(pair.b)}</td><td>${escapeHtml(pair.bName)}</td></tr>`).join("");
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
      Эталон: ${escapeHtml(result.reference || 'не указан')}<br>
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

function uploadFile(kind, input, bar, text) {
  const file = input.files?.[0];
  if (!file) return toast("Сначала выберите файл.");

  const form = new FormData();
  form.append("file", file, file.name);
  const xhr = new XMLHttpRequest();
  xhr.open("POST", `/api/upload/${kind}`, true);
  xhr.responseType = "json";

  input.disabled = true;
  bar.style.width = "0%";
  text.textContent = `Загрузка ${file.name}…`;

  xhr.upload.onprogress = (event) => {
    if (!event.lengthComputable) return;
    const percent = Math.round((event.loaded * 100) / event.total);
    bar.style.width = `${percent}%`;
    text.textContent = `${percent}% · ${fileSize(event.loaded)} / ${fileSize(event.total)}`;
  };

  xhr.onload = () => {
    input.disabled = false;
    const response = xhr.response || {};
    if (xhr.status >= 200 && xhr.status < 300) {
      bar.style.width = "100%";
      text.textContent = `Загружено: ${response.file || file.name}`;
      input.value = "";
      toast("Файл проверен и сохранён.");
      loadFiles();
    } else {
      bar.style.width = "0%";
      text.textContent = response.error || `Ошибка HTTP ${xhr.status}`;
      toast(text.textContent);
    }
  };

  xhr.onerror = () => {
    input.disabled = false;
    bar.style.width = "0%";
    text.textContent = "Ошибка сети при загрузке";
    toast(text.textContent);
  };

  xhr.send(form);
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

$("referenceUploadButton").addEventListener("click", () => uploadFile(
  "reference", $("referenceUploadInput"), $("referenceUploadBar"), $("referenceUploadText")
));
$("calculationUploadButton").addEventListener("click", () => uploadFile(
  "calculation", $("calculationUploadInput"), $("calculationUploadBar"), $("calculationUploadText")
));

$("referenceUploadInput").addEventListener("change", (event) => {
  $("referenceUploadText").textContent = event.target.files?.[0]?.name || "Файл не выбран";
});
$("calculationUploadInput").addEventListener("change", (event) => {
  $("calculationUploadText").textContent = event.target.files?.[0]?.name || "Файл не выбран";
});

$("refreshFilesButton").addEventListener("click", loadFiles);
$("refreshResultsButton").addEventListener("click", async () => {
  const files = await api("/api/calculations");
  const reports = files.filter((item) => item.file.endsWith(".json"));
  if (reports.length) loadResult(reports[reports.length - 1].file);
});

loadInitialStatus();
connectEvents();
loadDevice();
loadFiles();
setInterval(loadDevice, 10000);

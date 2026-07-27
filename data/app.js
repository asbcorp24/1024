const $ = (id) => document.getElementById(id);
let busy = false;
let lastResultLoaded = "";
let eventSource;

const referenceFieldIds = [
  "referenceName", "cableType", "revision", "deviceId", "operatorName",
  "mappingCrc32", "approvalStatus", "referenceComment"
];

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

function browserTimeParams() {
  const now = new Date();
  const timeZone = Intl.DateTimeFormat().resolvedOptions().timeZone || "Browser/Local";
  return new URLSearchParams({
    epochMs: String(now.getTime()),
    offsetMinutes: String(-now.getTimezoneOffset()),
    timeZone
  });
}

function setBusy(value) {
  busy = value;
  $("captureButton").disabled = value;
  $("testButton").disabled = value || !$("referenceSelect").value;
  $("referenceSelect").disabled = value;
  for (const id of referenceFieldIds) $(id).disabled = value;
}

function formatSeconds(ms) {
  return `${(ms / 1000).toFixed(1).replace('.', ',')} с`;
}

function formatStoredDate(value) {
  if (!value) return "не указана";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString("ru-RU");
}

function formatCrc32(value) {
  if (value === undefined || value === null) return "не указан";
  return Number(value).toString(16).toUpperCase().padStart(8, "0");
}

function approvalStatusLabel(status) {
  return ({
    draft: "Черновик",
    pending: "На утверждении",
    approved: "Утверждён",
    rejected: "Отклонён",
    archived: "Архивный"
  })[status] || status || "не указан";
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
    badge.textContent = `${device.deviceModel || device.hardware} · ${device.ip} · ${device.link ? 'LINK' : 'NO LINK'} · FW ${device.firmwareVersion || '?'}`;
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
    row.className = `file-item${item.valid === false ? " invalid-file" : ""}`;

    const info = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = item.name || item.file;
    const details = document.createElement("small");

    if (type === "reference") {
      details.innerHTML = item.valid
        ? `${escapeHtml(item.cableType)} · рев. ${escapeHtml(item.revision)} · ${escapeHtml(approvalStatusLabel(item.approvalStatus))}<br>` +
          `${escapeHtml(item.deviceId)} · ${escapeHtml(item.operator)} · ${escapeHtml(formatStoredDate(item.createdAtLocal))}<br>` +
          `CRC таблицы ${escapeHtml(formatCrc32(item.mappingCrc32))} · CRC матрицы ${escapeHtml(formatCrc32(item.matrixCrc32))} · ${escapeHtml(fileSize(item.size))}`
        : `Несовместимый или повреждённый эталон · ${escapeHtml(fileSize(item.size))}`;
    } else {
      details.textContent = `${fileSize(item.size)}${item.kind ? ` · ${item.kind}` : ""}`;
    }
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
    for (const item of references.filter((reference) => reference.valid)) {
      const option = document.createElement("option");
      option.value = item.file;
      option.textContent = `${item.name} · ${item.cableType} · рев. ${item.revision} · ${approvalStatusLabel(item.approvalStatus)}`;
      select.append(option);
    }
    if (references.some((item) => item.valid && item.file === selected)) select.value = selected;
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
    const storedTime = result.time || {};
    const reference = result.referenceMetadata || {};
    $("resultSummary").className = result.passed ? "result-ok" : "result-error";
    $("resultSummary").innerHTML = `
      <strong>${result.passed ? 'КАБЕЛЬ ИСПРАВЕН' : 'ОБНАРУЖЕНЫ ОТЛИЧИЯ'}</strong><br>
      Испытание: ${escapeHtml(formatStoredDate(storedTime.startedAtLocal))} · ${escapeHtml(storedTime.timeZone || 'часовой пояс не указан')}<br>
      Эталон: ${escapeHtml(reference.name || result.reference || 'не указан')} · ${escapeHtml(reference.cableType || '')} · рев. ${escapeHtml(reference.revision || '')}<br>
      Статус эталона: ${escapeHtml(approvalStatusLabel(reference.approvalStatus))}; прибор: ${escapeHtml(reference.deviceId || 'не указан')}; оператор эталона: ${escapeHtml(reference.operator || 'не указан')}<br>
      CRC таблицы: ${escapeHtml(formatCrc32(reference.mappingCrc32))}; CRC эталонной матрицы: ${escapeHtml(formatCrc32(reference.matrixCrc32))}<br>
      Обрывы: ${summary.missingLinks || 0}; паразитные связи: ${summary.extraLinks || 0};
      асимметрии: ${summary.asymmetricLinks || 0}; длительность: ${formatSeconds(result.elapsedMs || 0)}.
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

function referenceMetadataParams() {
  const required = [
    ["referenceName", "Введите название эталона."],
    ["cableType", "Введите тип кабельной сборки."],
    ["revision", "Введите ревизию."],
    ["deviceId", "Укажите прибор."],
    ["operatorName", "Укажите оператора."]
  ];
  for (const [id, message] of required) {
    if (!$(id).value.trim()) throw new Error(message);
  }

  const mappingCrc = $("mappingCrc32").value.trim();
  if (mappingCrc && !/^(?:0x)?[0-9a-f]{1,8}$/i.test(mappingCrc)) {
    throw new Error("CRC32 таблицы должен содержать до 8 hex-символов.");
  }

  const params = browserTimeParams();
  params.set("name", $("referenceName").value.trim());
  params.set("cableType", $("cableType").value.trim());
  params.set("revision", $("revision").value.trim());
  params.set("deviceId", $("deviceId").value.trim());
  params.set("operator", $("operatorName").value.trim());
  params.set("mappingCrc32", mappingCrc);
  params.set("approvalStatus", $("approvalStatus").value);
  params.set("comment", $("referenceComment").value.trim());
  return params;
}

$("captureButton").addEventListener("click", async () => {
  try {
    const params = referenceMetadataParams();
    localStorage.setItem("cableTesterDeviceId", $("deviceId").value.trim());
    localStorage.setItem("cableTesterOperator", $("operatorName").value.trim());
    await api(`/api/reference/capture?${params}`, { method: "POST" });
    setBusy(true);
    toast("Эталон v2 запущен. Дата и время получены из браузера.");
  } catch (error) { toast(error.message); }
});

$("testButton").addEventListener("click", async () => {
  const reference = $("referenceSelect").value;
  if (!reference) return toast("Выберите совместимый эталон v2.");
  try {
    const params = browserTimeParams();
    params.set("reference", reference);
    await api(`/api/test/start?${params}`, { method: "POST" });
    setBusy(true);
    toast("Проверка кабеля запущена. Время получено из браузера.");
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

$("deviceId").value = localStorage.getItem("cableTesterDeviceId") || "";
$("operatorName").value = localStorage.getItem("cableTesterOperator") || "";
loadInitialStatus();
connectEvents();
loadDevice();
loadFiles();
setInterval(loadDevice, 10000);

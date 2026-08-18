const $ = (id) => document.getElementById(id);
let busy = false;
let lastResultLoaded = "";
let eventSource;
let lastMatrixKind = "";
let lastReferenceLoaded = "";
let currentMatrixView = null;
let selectedMatrixPair = null;
let latestStatus = null;
let journalPage = 1;
let lastRefreshState = "";
let referenceJournalPage = 1;
let currentCalculations = [];
let currentReferences = [];
let currentResultForPrint = null;
let serviceUnlockEnabled = false;
let namedConnectionsExpanded = false;
let selectedGuideMark = "";

const JOURNAL_PAGE_SIZE = 20;
const HISTORY_LIMIT = 20;
const DEVICE_HISTORY_KEY = "cableTesterDeviceHistory";
const OPERATOR_HISTORY_KEY = "cableTesterOperatorHistory";

const referenceFieldIds = [
  "referenceName", "cableType", "revision", "deviceId", "operatorName",
  "mappingCrc32", "approvalStatus", "referenceComment"
];

function toast(message) {
  const el = $("toast");
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => {
    el.hidden = true;
  }, 4200);
}

function serviceUnlockMessage() {
  return "Сервисный тумблер выключен. Включите его для изменения эталонов, проверок и загрузки файлов.";
}

function readHistory(key) {
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.filter((item) => typeof item === "string" && item.trim()) : [];
  } catch {
    return [];
  }
}

function writeHistory(key, values) {
  localStorage.setItem(key, JSON.stringify(values.slice(0, HISTORY_LIMIT)));
}

function rememberHistoryValue(key, value) {
  const normalized = String(value ?? "").trim();
  if (!normalized) return;
  const unique = [normalized, ...readHistory(key).filter((item) => item !== normalized)];
  writeHistory(key, unique);
}

function renderSuggestionList(listId, values) {
  const list = $(listId);
  if (!list) return;
  list.innerHTML = "";
  for (const value of values) {
    const option = document.createElement("option");
    option.value = value;
    list.append(option);
  }
}

function refreshInputSuggestions() {
  renderSuggestionList("deviceSuggestions", readHistory(DEVICE_HISTORY_KEY));
  renderSuggestionList("operatorSuggestions", readHistory(OPERATOR_HISTORY_KEY));
}

function rememberCurrentMetadata() {
  rememberHistoryValue(DEVICE_HISTORY_KEY, $("deviceId")?.value);
  rememberHistoryValue(DEVICE_HISTORY_KEY, $("testDeviceId")?.value);
  rememberHistoryValue(OPERATOR_HISTORY_KEY, $("operatorName")?.value);
  rememberHistoryValue(OPERATOR_HISTORY_KEY, $("testOperatorName")?.value);
  refreshInputSuggestions();
}

function primeSuggestionsFromRecords(records, deviceField = "deviceId", operatorField = "operator") {
  if (!Array.isArray(records)) return;
  for (const item of records) {
    rememberHistoryValue(DEVICE_HISTORY_KEY, item?.[deviceField]);
    rememberHistoryValue(OPERATOR_HISTORY_KEY, item?.[operatorField]);
  }
}

async function api(url, options = {}) {
  const response = await fetch(url, { cache: "no-store", ...options });
  const text = await response.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    data = text;
  }
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

function contactNumber(pin) {
  return Number(pin) + 1;
}

function contactLabel(pin) {
  return `Контакт ${contactNumber(pin)}`;
}

function logicalPinText(value) {
  return String(value ?? "").trim();
}

function pairNamedEndpoint(mark, logicalPin, pin) {
  const markText = String(mark ?? "").trim();
  const logicalText = logicalPinText(logicalPin);
  if (markText && logicalText) return `${markText}/${logicalText}`;
  if (markText) return `${markText}/${contactNumber(pin)}`;
  if (logicalText) return logicalText;
  return contactLabel(pin);
}

function pairDisplayText(pair) {
  const wire = pair?.wireName ? `Провод ${pair.wireName}` : "Связь";
  return `${wire}: ${pairNamedEndpoint(pair?.markA, pair?.localPinA, pair?.a)} ↔ ${pairNamedEndpoint(pair?.markB, pair?.localPinB, pair?.b)}`;
}

function pairMetaText(pair) {
  const items = [];
  if (pair?.wireName) items.push(`провод ${pair.wireName}`);
  if (pair?.markA || pair?.localPinA) items.push(`A=${pairNamedEndpoint(pair?.markA, pair?.localPinA, pair?.a)}`);
  if (pair?.markB || pair?.localPinB) items.push(`B=${pairNamedEndpoint(pair?.markB, pair?.localPinB, pair?.b)}`);
  items.push(`каналы=${contactNumber(pair?.a)}↔${contactNumber(pair?.b)}`);
  if (pair?.note) items.push(`примечание: ${pair.note}`);
  return items.join(", ");
}

function normalizePairKey(a, b) {
  const left = Math.min(Number(a), Number(b));
  const right = Math.max(Number(a), Number(b));
  return `${left}:${right}`;
}

function buildAnnotationMap(view) {
  const map = new Map();
  const annotations = Array.isArray(view?.annotations) ? view.annotations : [];
  for (const item of annotations) {
    map.set(normalizePairKey(item.a, item.b), item);
  }
  return map;
}

function buildContactAnnotationMap(view) {
  const map = new Map();
  const annotations = Array.isArray(view?.annotations) ? view.annotations : [];
  const ensureEntry = (pin) => {
    const key = Number(pin);
    if (!map.has(key)) map.set(key, { marks: new Set(), wires: new Set() });
    return map.get(key);
  };

  for (const item of annotations) {
    const entryA = ensureEntry(item.a);
    const entryB = ensureEntry(item.b);
    if (item.markA) entryA.marks.add(String(item.markA));
    if (item.markB) entryB.marks.add(String(item.markB));
    if (item.wireName) {
      entryA.wires.add(String(item.wireName));
      entryB.wires.add(String(item.wireName));
    }
  }
  return map;
}

function summarizeSet(values, limit = 3) {
  const list = Array.from(values || []);
  if (!list.length) return "";
  if (list.length <= limit) return list.join("/");
  return `${list.slice(0, limit).join("/")}/…`;
}

function enrichPairsWithAnnotations(pairs, annotationMap, contactAnnotationMap = new Map()) {
  return (Array.isArray(pairs) ? pairs : []).map((pair) => {
    if (pair?.wireName || pair?.markA || pair?.markB || pair?.note) return pair;
    const annotation = annotationMap.get(normalizePairKey(pair?.a, pair?.b));
    if (annotation) return { ...pair, ...annotation };

    const contactA = contactAnnotationMap.get(Number(pair?.a));
    const contactB = contactAnnotationMap.get(Number(pair?.b));
    const inferredMarkA = summarizeSet(contactA?.marks);
    const inferredMarkB = summarizeSet(contactB?.marks);
    const inferredWireA = summarizeSet(contactA?.wires, 2);
    const inferredWireB = summarizeSet(contactB?.wires, 2);

    if (!inferredMarkA && !inferredMarkB && !inferredWireA && !inferredWireB) return pair;
    return {
      ...pair,
      markA: pair?.markA || inferredMarkA,
      markB: pair?.markB || inferredMarkB,
      note: pair?.note || ((inferredWireA || inferredWireB)
        ? `вне эталонной пары; A из проводов ${inferredWireA || "?"}, B из проводов ${inferredWireB || "?"}`
        : "вне эталонной пары")
    };
  });
}

function pairsHaveAnnotations(pairs) {
  return (Array.isArray(pairs) ? pairs : []).some((pair) => pair?.wireName || pair?.markA || pair?.markB || pair?.note);
}

function updateNamedConnectionsToggle() {
  const button = $("toggleNamedConnectionsButton");
  const block = $("namedConnectionsBlock");
  if (!button || !block) return;
  block.hidden = !namedConnectionsExpanded;
  button.textContent = namedConnectionsExpanded ? "Скрыть провода и маркировку" : "Показать провода и маркировку";
}

function attachPairRowHandlers(target) {
  target.querySelectorAll("tbody tr[data-a][data-b]").forEach((row) => {
    row.classList.add("pair-row");
    row.addEventListener("click", () => {
      target.querySelectorAll("tbody tr.pair-row").forEach((item) => item.classList.remove("active"));
      row.classList.add("active");
      selectedMatrixPair = {
        a: Number(row.dataset.a),
        b: Number(row.dataset.b)
      };
      if (currentMatrixView) renderReferenceGuide(currentMatrixView);
    });
  });
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

  const overlay = $("busyOverlay");
  if (overlay) {
    overlay.hidden = !value;
    document.body.classList.toggle("busy-ui", value);
  }
}

function applyServiceProtection(device) {
  const previous = serviceUnlockEnabled;
  serviceUnlockEnabled = Boolean(device?.serviceUnlockEnabled);

  if ($("captureButton")) $("captureButton").hidden = !serviceUnlockEnabled;
  if ($("adminPageLink")) $("adminPageLink").hidden = !serviceUnlockEnabled;
  if ($("openAdminPageButton")) $("openAdminPageButton").hidden = !serviceUnlockEnabled;
  if ($("toggleReferenceFormButton")) $("toggleReferenceFormButton").hidden = !serviceUnlockEnabled;
  if ($("toggleUploadPanelButton")) $("toggleUploadPanelButton").hidden = !serviceUnlockEnabled;
  if ($("referenceFormPanel") && !serviceUnlockEnabled) $("referenceFormPanel").hidden = true;
  if ($("uploadPanel") && !serviceUnlockEnabled) $("uploadPanel").hidden = true;
  if ($("referenceUploadButton")) $("referenceUploadButton").hidden = !serviceUnlockEnabled;
  if ($("calculationUploadButton")) $("calculationUploadButton").hidden = !serviceUnlockEnabled;
  if ($("referenceUploadInput")) $("referenceUploadInput").hidden = !serviceUnlockEnabled;
  if ($("calculationUploadInput")) $("calculationUploadInput").hidden = !serviceUnlockEnabled;

  if (previous !== serviceUnlockEnabled) {
    renderJournal(currentCalculations);
    renderReferenceJournal(currentReferences);
  }
}

setBusy(false);

function setupCollapsiblePanel(buttonId, panelId) {
  const button = $(buttonId);
  const panel = $(panelId);
  if (!button || !panel) return;

  const applyState = (expanded) => {
    panel.hidden = !expanded;
    button.setAttribute("aria-expanded", expanded ? "true" : "false");
    button.textContent = expanded ? "Скрыть" : "Открыть";
  };

  applyState(!panel.hidden);
  button.addEventListener("click", () => {
    applyState(panel.hidden);
  });
}

function setupStorageUi() {
  const duplicateLists = document.querySelector(".files-columns");
  if (duplicateLists) duplicateLists.hidden = true;

  const calculationToolbar = $("journalPagination")?.parentElement;
  if (calculationToolbar && !$("calculationCsvButton")) {
    const button = document.createElement("button");
    button.id = "calculationCsvButton";
    button.className = "secondary";
    button.type = "button";
    button.textContent = "Выгрузить CSV";
    calculationToolbar.prepend(button);
  }

  if (!$("referenceJournalTable")) {
    const storageSection = $("refreshFilesButton")?.closest("section");
    if (storageSection) {
      const section = document.createElement("section");
      section.className = "card wide";
      section.innerHTML = `
        <div class="card-head">
          <h2>Журнал эталонов</h2>
          <div id="referenceJournalSummary" class="badge neutral">0 записей</div>
        </div>
        <div class="journal-toolbar">
          <label class="journal-sort">
            <span>Сортировка</span>
            <select id="referenceJournalSort">
              <option value="date_desc">Дата: сначала новые</option>
              <option value="date_asc">Дата: сначала старые</option>
              <option value="name_asc">Название: А-Я</option>
              <option value="name_desc">Название: Я-А</option>
            </select>
          </label>
          <div class="journal-toolbar-actions">
            <button id="referenceCsvButton" class="secondary" type="button">Выгрузить CSV</button>
            <div id="referenceJournalPagination" class="journal-pagination" hidden></div>
          </div>
        </div>
        <div id="referenceJournalTable" class="empty">Записей пока нет.</div>
      `;
      storageSection.insertAdjacentElement("afterend", section);
    }
  }
}

function csvEscape(value) {
  const text = String(value ?? "");
  return `"${text.replaceAll('"', '""')}"`;
}

function downloadTextFile(fileName, content, mimeType) {
  const normalizedType = String(mimeType || "");
  const payload = normalizedType.includes("charset=utf-8") ? `\uFEFF${content}` : content;
  const blob = new Blob([payload], { type: mimeType });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = fileName;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function updateBusyOverlay(status) {
  const overlay = $("busyOverlay");
  const title = $("busyOverlayTitle");
  const text = $("busyOverlayText");
  if (!overlay || !title || !text) return;

  const state = status?.state || "idle";
  title.textContent = ({
    preparing: "Подготовка теста",
    scanning: "Идет тестирование",
    analyzing: "Анализ результата",
    saving: "Сохранение расчета"
  })[state] || "Идет обработка";

  text.textContent = status?.message ||
    ({
      preparing: "Подготавливаем линии и эталон.",
      scanning: "Проверяем соединения, пожалуйста подождите.",
      analyzing: "Сравниваем измерение с эталоном.",
      saving: "Обновляем хранилище и сохраняем файлы."
    })[state] ||
    "Подождите, идет обработка.";
}

function formatSeconds(ms) {
  return `${(ms / 1000).toFixed(1).replace(".", ",")} с`;
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

function ensureResultOpenTitle() {
  const summary = $("resultSummary");
  if (!summary) return null;

  let title = $("resultOpenTitle");
  if (!title) {
    title = document.createElement("div");
    title.id = "resultOpenTitle";
    title.className = "result-open-title";
    title.hidden = true;
    summary.insertAdjacentElement("beforebegin", title);
  }

  return title;
}

function revealOpenedResult(fileName) {
  const title = ensureResultOpenTitle();
  const summary = $("resultSummary");
  const card = summary?.closest("section.card");

  if (title) {
    title.textContent = `Открыт отчёт: ${fileName}`;
    title.hidden = false;
  }

  if (card) {
    card.classList.remove("result-card-highlight");
    void card.offsetWidth;
    card.classList.add("result-card-highlight");
    clearTimeout(revealOpenedResult.timer);
    revealOpenedResult.timer = setTimeout(() => {
      card.classList.remove("result-card-highlight");
    }, 1800);
    card.scrollIntoView({ behavior: "smooth", block: "start" });
  } else if (summary) {
    summary.scrollIntoView({ behavior: "smooth", block: "start" });
  }
}

function ensurePrintButton() {
  const refreshButton = $("refreshResultsButton");
  if (!refreshButton || $("printResultButton")) return;

  const actions = document.createElement("div");
  actions.className = "card-head-actions";

  const printButton = document.createElement("button");
  printButton.id = "printResultButton";
  printButton.className = "secondary";
  printButton.type = "button";
  printButton.disabled = true;
  printButton.textContent = "Печать";

  const parent = refreshButton.parentElement;
  if (!parent) return;
  parent.insertBefore(actions, refreshButton);
  actions.append(printButton, refreshButton);
}

function buildPrintPairsRows(title, pairs) {
  const list = Array.isArray(pairs) ? pairs : [];
  if (!list.length) return "";
  return list.map((pair, index) => `
    <tr>
      <td>${index + 1}</td>
      <td>${escapeHtml(pairNamedEndpoint(pair?.markA, pair?.localPinA, pair?.source ?? pair?.a))}</td>
      <td>${escapeHtml(pairNamedEndpoint(pair?.markB, pair?.localPinB, pair?.target ?? pair?.b))}</td>
      <td>${escapeHtml(title)}</td>
      <td>${escapeHtml(pair?.wireName || "—")}</td>
      <td>${escapeHtml(pairMetaText(pair) || "—")}</td>
      <td>${escapeHtml(String(pair.delayUs ?? pair.delayMs ?? "—"))}</td>
    </tr>
  `).join("");
}

function printCurrentResult() {
  if (!currentResultForPrint) {
    toast("Сначала откройте нужный результат.");
    return;
  }

  const result = currentResultForPrint;
  const summary = result.summary || {};
  const time = result.time || {};
  const reference = result.referenceMetadata || {};
  const measurement = result.measurementMetadata || {};
  const printPairs = result.printPairs || {};
  const discrepancyRows =
    buildPrintPairsRows("Обрыв", printPairs.missing || result.missing) +
    buildPrintPairsRows("Паразитная связь", printPairs.extra || result.extra) +
    buildPrintPairsRows("Асимметрия", printPairs.asymmetric || result.asymmetric);

  const html = `<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <title>Отчет кабельной сборки ${escapeHtml(lastResultLoaded || "")}</title>
  <style>
    body { font-family: "Segoe UI", Arial, sans-serif; margin: 24px; color: #111; }
    h1, h2 { margin: 0 0 12px; }
    .meta { width: 100%; border-collapse: collapse; margin: 0 0 18px; }
    .meta td { border: 1px solid #777; padding: 8px 10px; vertical-align: top; }
    .meta td.label { width: 220px; font-weight: 700; background: #f3f3f3; }
    .summary { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin: 0 0 18px; }
    .summary div { border: 1px solid #777; padding: 10px; }
    .summary b { display: block; font-size: 20px; margin-bottom: 4px; }
    table.report { width: 100%; border-collapse: collapse; margin-top: 10px; }
    table.report th, table.report td { border: 1px solid #777; padding: 8px 10px; text-align: left; }
    table.report th { background: #f3f3f3; }
    .status { font-weight: 700; padding: 8px 10px; border: 1px solid #777; margin: 0 0 18px; }
    .signatures { margin-top: 28px; display: grid; grid-template-columns: 1fr 1fr; gap: 24px; }
    .sign { padding-top: 32px; border-bottom: 1px solid #333; }
    @media print { body { margin: 12mm; } }
  </style>
</head>
<body>
  <h1>Протокол проверки кабельной сборки</h1>
  <div class="status">Итог: ${escapeHtml(result.passed ? "ГОДЕН" : "НЕ ГОДЕН")} | Файл: ${escapeHtml(lastResultLoaded || "не указан")}</div>
  <table class="meta">
    <tr><td class="label">Дата и время проверки</td><td>${escapeHtml(formatStoredDate(time.startedAtLocal || ""))}</td></tr>
    <tr><td class="label">Часовой пояс</td><td>${escapeHtml(time.timeZone || "не указан")}</td></tr>
    <tr><td class="label">Тип сборки</td><td>${escapeHtml(measurement.cableType || "не указан")}</td></tr>
    <tr><td class="label">Прибор</td><td>${escapeHtml(measurement.deviceId || "не указан")}</td></tr>
    <tr><td class="label">Оператор</td><td>${escapeHtml(measurement.operator || "не указан")}</td></tr>
    <tr><td class="label">Эталон</td><td>${escapeHtml(reference.name || result.reference || "не указан")}</td></tr>
    <tr><td class="label">Ревизия эталона</td><td>${escapeHtml(reference.revision || "не указана")}</td></tr>
    <tr><td class="label">Статус эталона</td><td>${escapeHtml(approvalStatusLabel(reference.approvalStatus))}</td></tr>
  </table>
  <div class="summary">
    <div><b>${summary.expectedLinks || 0}</b><span>Ожидалось связей</span></div>
    <div><b>${summary.measuredLinks || 0}</b><span>Измерено связей</span></div>
    <div><b>${summary.missingLinks || 0}</b><span>Обрывы</span></div>
    <div><b>${summary.extraLinks || 0}</b><span>Паразитные связи</span></div>
    <div><b>${summary.asymmetricLinks || 0}</b><span>Асимметрия</span></div>
    <div><b>${summary.stuckLowPins || 0}</b><span>Залипшие линии</span></div>
    <div><b>${summary.sourceDriveErrors || 0}</b><span>Ошибки источника</span></div>
    <div><b>${formatSeconds(result.elapsedMs || 0)}</b><span>Длительность</span></div>
  </div>
  <h2>Выявленные несоответствия</h2>
  ${discrepancyRows ? `<table class="report"><thead><tr><th>№</th><th>Точка A</th><th>Точка B</th><th>Тип</th><th>Провод</th><th>Маркировка и каналы</th><th>Задержка</th></tr></thead><tbody>${discrepancyRows}</tbody></table>` : "<div class=\"status\">Несоответствия не обнаружены.</div>"}
  <div class="signatures">
    <div><div>Проверил</div><div class="sign"></div></div>
    <div><div>Принял</div><div class="sign"></div></div>
  </div>
</body>
</html>`;

  const printWindow = window.open("", "_blank", "width=980,height=780");
  if (!printWindow) {
    toast("Браузер заблокировал окно печати.");
    return;
  }
  printWindow.document.open();
  printWindow.document.write(html);
  printWindow.document.close();
  printWindow.focus();
  setTimeout(() => printWindow.print(), 250);
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
  const previousStatus = latestStatus;
  latestStatus = status || null;
  const active = ["preparing", "scanning", "analyzing", "saving"].includes(status.state);
  setBusy(active);
  if (active) updateBusyOverlay(status);
  $("progressBar").style.width = `${status.progress || 0}%`;
  $("progressText").textContent = `${status.currentSource || 0} / ${status.totalSources || 1024}`;
  $("elapsedText").textContent = formatSeconds(status.elapsedMs || 0);
  $("statusMessage").textContent = status.message || "";

  const badge = $("stateBadge");
  badge.textContent = ({
    idle: "Ожидание",
    preparing: "Подготовка",
    scanning: "Измерение",
    analyzing: "Расчёт",
    saving: "Сохранение",
    completed: "Завершено",
    failed: "Ошибка"
  })[status.state] || status.state;
  badge.className = `badge ${
    status.state === "completed" ? "ok" :
    status.state === "failed" ? "error" :
    active ? "busy" : "neutral"
  }`;

  const s = status.statistics || {};
  for (const key of ["expectedLinks", "measuredLinks", "missingLinks", "extraLinks", "asymmetricLinks", "stuckLowPins"]) {
    $(key).textContent = s[key] ?? 0;
  }

  const stateChanged = previousStatus?.state !== status.state;
  const resultChanged = previousStatus?.lastResult !== status.lastResult;

  if (status.lastResult && status.state === "completed" && status.lastResult !== lastResultLoaded) {
    loadResult(status.lastResult);
    loadFiles();
    loadDevice();
    lastRefreshState = status.state;
  } else if ((status.state === "saving" || status.state === "completed" || status.state === "failed") &&
             (stateChanged || resultChanged || lastRefreshState !== status.state)) {
    loadFiles();
    loadDevice();
    lastRefreshState = status.state;
  } else if (active) {
    lastRefreshState = "";
  }
}

async function loadInitialStatus() {
  try {
    updateStatus(await api("/api/status"));
  } catch (error) {
    $("statusMessage").textContent = `Нет связи с устройством: ${error.message}`;
  }
}

function connectEvents() {
  if (eventSource) eventSource.close();
  eventSource = new EventSource("/api/events");
  eventSource.addEventListener("status", (event) => {
    try {
      updateStatus(JSON.parse(event.data));
    } catch (error) {
      console.error("Некорректное SSE-событие", error);
    }
  });
  eventSource.addEventListener("files", () => loadFiles());
  eventSource.onopen = () => document.body.classList.remove("offline");
  eventSource.onerror = () => {
    document.body.classList.add("offline");
    $("statusMessage").textContent = "SSE-соединение восстанавливается…";
  };
}

function renderTimeSyncStatus(device) {
  const el = $("timeSyncStatus");
  if (!el) return;
  if (device?.clockSynced) {
    el.textContent =
      `Синхронизировано: ${formatStoredDate(device.clockLastSyncLocal)} · ${device.clockTimeZone || "часовой пояс не указан"} · UTC ${device.clockLastSyncUtc || "не указан"}`;
  } else {
    el.textContent = "Время на плате ещё не синхронизировано.";
  }
}

function renderMatrixLegend(items) {
  const legend = $("matrixLegend");
  legend.innerHTML = "";
  for (const item of items || []) {
    const el = document.createElement("div");
    el.className = "matrix-legend-item";
    el.innerHTML = `<span class="matrix-swatch" style="background:${escapeHtml(item.color)}"></span><span>${escapeHtml(item.label)}</span>`;
    legend.append(el);
  }
}

function renderMatrixLegendNote(view) {
  const target = $("matrixLegendNote");
  if (!target) return;

  const items = view?.kind === "result"
    ? [
        { color: "#1f7a3f", label: "Совпадение" },
        { color: "#c0392b", label: "Обрыв" },
        { color: "#1f6fd6", label: "Паразитная связь" },
        { color: "#d9aa00", label: "Асимметрия" },
        { color: "#0f4c81", label: "Выбранная пара" }
      ]
    : [
        { color: "#1f7a3f", label: "Есть связь" },
        { color: "#0f4c81", label: "Выбранная пара" }
      ];

  target.innerHTML = items
    .map((item) =>
      `<div class="matrix-note-item"><span class="matrix-note-dot" style="background:${escapeHtml(item.color)}"></span><span>${escapeHtml(item.label)}</span></div>`)
    .join("");
}

function formatMicroseconds(value) {
  return `${Number(value || 0)} мкс`;
}

function renderMatrixMeta(view) {
  const target = $("matrixMeta");
  if (!target) return;

  const physical = Number(view?.connectionCount || 0);
  const directed = Number(view?.directedConnectionCount || 0);
  const activeCells = Number(view?.activeCellCount || 0);
  const settleUs = Number(view?.measurementSettleUs || 0);
  const scopeLabel = view?.scopeLabel || "Карта показывает укрупнённые пары модулей.";
  const annotationsLoaded = Array.isArray(view?.annotations) ? view.annotations.length : 0;

  target.className = "message matrix-meta";
  target.innerHTML = `
    <strong>Сводка</strong>
    Физических связей: ${escapeHtml(physical)}<br>
    Направленных срабатываний: ${escapeHtml(directed)}<br>
    Закрашенных клеток на карте: ${escapeHtml(activeCells)}<br>
    Выдержка перед чтением: ${escapeHtml(formatMicroseconds(settleUs))}<br>
    annotations loaded: ${escapeHtml(annotationsLoaded)}<br>
    ${escapeHtml(scopeLabel)}
  `;
}

function statusChip(kind, label) {
  const safeKind = ["ok", "missing", "extra", "asymmetric"].includes(kind) ? kind : "ok";
  return `<span class="status-chip ${safeKind}"><span class="status-chip-dot"></span><span>${escapeHtml(label)}</span></span>`;
}

function renderMatrixConnections(view) {
  const target = $("matrixConnections");
  if (!target) return;

  const items = Array.isArray(view?.connections) ? view.connections : [];
  if (!items.length) {
    target.className = "empty";
    target.textContent = "Связи для текущей матрицы не найдены.";
    return;
  }

  const rows = items
    .map((item) =>
      `<tr data-a="${escapeHtml(item.a)}" data-b="${escapeHtml(item.b)}"><td>${escapeHtml(contactLabel(item.a))}</td><td>${escapeHtml(contactLabel(item.b))}</td><td>${escapeHtml(formatMicroseconds(item.settleUs))}</td></tr>`)
    .join("");

  target.className = "";
  target.innerHTML =
    `<div class="table-wrap"><table><thead><tr><th>Контакт A</th><th>Контакт B</th><th>Выдержка</th></tr></thead><tbody>${rows}</tbody></table></div>` +
    `${view?.connectionsTruncated ? "<p>Список сокращён. Полное количество показано в сводке.</p>" : ""}`;
  attachPairRowHandlers(target);
}

function renderMatrixAxes(size) {
  const top = $("matrixAxisTop");
  const left = $("matrixAxisLeft");
  top.innerHTML = "";
  left.innerHTML = "";

  for (let index = 0; index < size; index += 1) {
    const topLabel = document.createElement("span");
    const leftLabel = document.createElement("span");
    const label = String(index);
    const major = index % 8 === 0;

    topLabel.textContent = major ? label : "·";
    leftLabel.textContent = major ? label : "·";
    topLabel.className = major ? "major" : "muted";
    leftLabel.className = major ? "major" : "muted";

    top.append(topLabel);
    left.append(leftLabel);
  }
}

function renderMatrix(view) {
  $("matrixTitle").textContent = view?.kind === "reference" ? "Эталон: матрица связей 64×64" : "Результат: матрица связей 64×64";
  $("matrixSubtitle").textContent = view?.subtitle || view?.title || "Матрица связей по 64 модулям.";
  renderMatrixLegend(view?.legend || []);
  renderMatrixLegendNote(view);
  renderMatrixMeta(view);
  renderMatrixConnections(view);

  const canvas = $("matrixCanvas");
  const ctx = canvas.getContext("2d");
  const size = Number(view?.size || 64);
  renderMatrixAxes(size);
  const scale = Math.floor(canvas.width / size);
  const colors = new Map((view?.legend || []).map((item) => [item.code, item.color]));
  const cells = Array.isArray(view?.cells) ? view.cells : [];

  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  for (let row = 0; row < size; row += 1) {
    for (let col = 0; col < size; col += 1) {
      const code = Number(cells[row * size + col] ?? 0);
      ctx.fillStyle = colors.get(code) || "#e2e8f0";
      ctx.fillRect(col * scale, row * scale, scale, scale);
    }
  }

  ctx.strokeStyle = "rgba(23,32,51,0.08)";
  ctx.lineWidth = 1;
  for (let line = 0; line <= size; line += 1) {
    const pos = line * scale + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, pos);
    ctx.lineTo(size * scale, pos);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(pos, 0);
    ctx.lineTo(pos, size * scale);
    ctx.stroke();
  }

  ctx.strokeStyle = "rgba(23,32,51,0.22)";
  ctx.lineWidth = 1.5;
  for (let line = 0; line <= size; line += 8) {
    const pos = line * scale + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, pos);
    ctx.lineTo(size * scale, pos);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(pos, 0);
    ctx.lineTo(pos, size * scale);
    ctx.stroke();
  }
}

function renderMatrixAxes(size) {
  const top = $("matrixAxisTop");
  const left = $("matrixAxisLeft");
  top.innerHTML = "";
  left.innerHTML = "";

  for (let index = 0; index < size; index += 1) {
    const topLabel = document.createElement("span");
    const leftLabel = document.createElement("span");
    const label = String(index);
    const major = index % 8 === 0;

    topLabel.textContent = major ? label : "В·";
    leftLabel.textContent = major ? label : "В·";
    topLabel.className = major ? "major" : "muted";
    leftLabel.className = major ? "major" : "muted";

    top.append(topLabel);
    left.append(leftLabel);
  }
}

function renderMatrix(view) {
  currentMatrixView = view;
  renderNamedConnectionsBlock(view);
  renderReferenceGuide(view);
}

function buildMatrixRows(view) {
  if (view?.kind === "result") {
    const rows = [];
    for (const item of view?.missingConnections || []) rows.push({ ...item, kind: "missing", label: "Обрыв" });
    for (const item of view?.extraConnections || []) rows.push({ ...item, kind: "extra", label: "Лишняя связь" });
    for (const item of view?.asymmetricConnections || []) rows.push({ ...item, kind: "asymmetric", label: "Асимметрия" });
    if (rows.length) return rows;
  }
  return (view?.connections || []).map((item) => ({ ...item, kind: "ok", label: "Связь" }));
}

function buildReferenceGuideAssignments(view) {
  const assignments = new Map();
  const items = Array.isArray(view?.annotations) && view.annotations.length
    ? view.annotations
    : buildMatrixRows(view);

  const addEndpoint = (pin, mark, localPin, wireName, note) => {
    const physicalPin = Number(pin);
    if (!Number.isInteger(physicalPin) || physicalPin < 0 || physicalPin >= 1024) return;
    const normalizedMark = String(mark ?? "").trim();
    const endpoint = pairNamedEndpoint(mark, localPin, physicalPin);
    const metaParts = [];
    if (wireName) metaParts.push(`провод ${wireName}`);
    if (note) metaParts.push(note);
    const meta = metaParts.join(" · ");
    if (!assignments.has(physicalPin)) {
      assignments.set(physicalPin, {
        pin: physicalPin,
        mark: normalizedMark,
        endpoint,
        metas: meta ? new Set([meta]) : new Set()
      });
      return;
    }
    const existing = assignments.get(physicalPin);
    if (!existing.mark && normalizedMark) existing.mark = normalizedMark;
    if (existing.endpoint !== endpoint) existing.endpoint = `${existing.endpoint} / ${endpoint}`;
    if (meta) existing.metas.add(meta);
  };

  for (const item of items) {
    addEndpoint(item?.a, item?.markA, item?.localPinA, item?.wireName, item?.note);
    addEndpoint(item?.b, item?.markB, item?.localPinB, item?.wireName, item?.note);
  }

  return Array.from(assignments.values()).sort((left, right) => left.pin - right.pin);
}

function stringHash(value) {
  let hash = 0;
  const text = String(value ?? "");
  for (let index = 0; index < text.length; index += 1) {
    hash = ((hash << 5) - hash + text.charCodeAt(index)) | 0;
  }
  return Math.abs(hash);
}

function harnessColor(mark) {
  const text = String(mark ?? "").trim();
  if (!text) {
    return {
      bg: "#dbeafe",
      border: "#60a5fa",
      pin: "#1d4ed8",
      text: "#1e3a8a",
      meta: "#334155"
    };
  }

  const hue = stringHash(text) % 360;
  return {
    bg: `hsl(${hue} 88% 86%)`,
    border: `hsl(${hue} 70% 58%)`,
    pin: `hsl(${hue} 82% 28%)`,
    text: `hsl(${hue} 58% 18%)`,
    meta: `hsl(${hue} 34% 30%)`
  };
}

function renderReferenceGuideLegend(assignments) {
  const legend = $("referenceGuideLegend");
  if (!legend) return;

  const marks = Array.from(new Set((Array.isArray(assignments) ? assignments : [])
    .map((item) => String(item?.mark || "").trim())
    .filter(Boolean)))
    .sort((left, right) => left.localeCompare(right, "ru"));

  if (!marks.length) {
    selectedGuideMark = "";
    legend.innerHTML = '<div class="empty">Легенда по жгутам недоступна.</div>';
    return;
  }

  if (selectedGuideMark && !marks.includes(selectedGuideMark)) selectedGuideMark = "";

  legend.innerHTML = marks.map((mark) => {
    const colors = harnessColor(mark);
    return `
      <button type="button" class="reference-guide-legend-item ${selectedGuideMark === mark ? "active" : ""}" data-guide-mark="${escapeHtml(mark)}">
        <span class="reference-guide-legend-swatch" style="background:${escapeHtml(colors.bg)};border-color:${escapeHtml(colors.border)};"></span>
        <span>${escapeHtml(mark)}</span>
      </button>
    `;
  }).join("");

  legend.querySelectorAll("[data-guide-mark]").forEach((button) => {
    button.addEventListener("click", () => {
      const mark = button.dataset.guideMark || "";
      selectedGuideMark = selectedGuideMark === mark ? "" : mark;
      if (currentMatrixView) renderReferenceGuide(currentMatrixView);
    });
  });
}

function renderReferenceGuide(view) {
  const summary = $("referenceGuideSummary");
  const grid = $("referenceGuideGrid");
  if (!summary || !grid) return;

  const assignments = buildReferenceGuideAssignments(view);
  if (!assignments.length) {
    summary.textContent = "Для выбранного эталона нет данных по жгутам и контактам.";
    renderReferenceGuideLegend([]);
    grid.innerHTML = '<div class="empty">Подсказка по блокам недоступна.</div>';
    return;
  }

  renderReferenceGuideLegend(assignments);

  const blocks = Array.from({ length: 16 }, (_, index) => ({
    index,
    items: []
  }));

  for (const item of assignments) {
    const blockIndex = Math.floor(item.pin / 64);
    const positionInBlock = (item.pin % 64) + 1;
    blocks[blockIndex].items.push({
      ...item,
      positionInBlock
    });
  }

  summary.textContent = `Подсказка по раскладке: задействовано ${assignments.length} физических каналов. Ниже показаны все 16 блоков по 64 контакта и что в них вставлять.`;
  grid.innerHTML = blocks.map((block) => {
    const start = block.index * 64 + 1;
    const end = start + 63;
    const itemMap = new Map(block.items.map((item) => [item.positionInBlock, item]));
    const rows = Array.from({ length: 64 }, (_, positionIndex) => {
      const position = positionIndex + 1;
      const item = itemMap.get(position);
      const itemMark = String(item?.mark || "").trim();
      const colors = harnessColor(itemMark);
      const muted = Boolean(selectedGuideMark) && item && itemMark !== selectedGuideMark;
      const focused = Boolean(selectedGuideMark) && item && itemMark === selectedGuideMark;
      const selected = item && (
        Number(item.pin) === Number(selectedMatrixPair?.a) ||
        Number(item.pin) === Number(selectedMatrixPair?.b)
      );
      return `
        <div class="reference-guide-cell ${item ? "occupied" : ""} ${muted ? "muted" : ""} ${focused ? "focused" : ""} ${selected ? "selected" : ""}" style="${item ? `--guide-bg:${colors.bg};--guide-border:${colors.border};--guide-pin:${colors.pin};--guide-text:${colors.text};--guide-meta:${colors.meta};` : ""}" title="${escapeHtml(item ? `${item.endpoint}${item.metas.size ? ` | ${Array.from(item.metas).join(" | ")}` : ""}` : `Позиция ${position} не используется`)}">
          <div class="reference-guide-cell-pin">${escapeHtml(String(position))}</div>
          ${item ? `
            <div class="reference-guide-cell-body">
              <div class="reference-guide-cell-endpoint">${escapeHtml(item.endpoint)}</div>
              <div class="reference-guide-cell-meta">${escapeHtml(String(item.pin + 1))}</div>
            </div>
          ` : '<div class="reference-guide-cell-empty">—</div>'}
        </div>
      `;
    }).join("");

    return `
      <div class="reference-guide-card">
        <div class="reference-guide-head">
          <strong>Блок ${escapeHtml(String(block.index + 1))}</strong>
          <span>Каналы ${escapeHtml(String(start))}–${escapeHtml(String(end))}</span>
        </div>
        <div class="reference-guide-body">
          <div class="reference-guide-panel">${rows}</div>
        </div>
      </div>
    `;
  }).join("");
}

function renderNamedConnectionsBlock(view) {
  const summary = $("namedConnectionsSummary");
  const table = $("namedConnectionsTable");
  if (!summary || !table) return;

  const items = buildMatrixRows(view).filter((item) => item.wireName || item.markA || item.markB || item.note);
  const kindLabel = view?.kind === "reference" ? "эталона" : view?.kind === "result" ? "результата" : "данных";

  if (!items.length) {
    summary.textContent = `Для текущего ${kindLabel} нет сохранённых проводов или маркировок.`;
    table.className = "empty";
    table.textContent = "Нет данных по проводам.";
    return;
  }

  summary.textContent = `Показано связей с именами проводов и маркировкой: ${items.length}.`;
  const rows = items.map((item, index) => `
    <tr data-a="${escapeHtml(item.a)}" data-b="${escapeHtml(item.b)}" data-kind="${escapeHtml(item.kind || "ok")}">
      <td>${index + 1}</td>
      <td>${statusChip(item.kind || "ok", item.label || "Связь")}</td>
      <td class="wrap">${escapeHtml(item.wireName || "—")}</td>
      <td class="wrap">${escapeHtml(pairNamedEndpoint(item.markA, item.localPinA, item.a))}</td>
      <td class="wrap">${escapeHtml(pairNamedEndpoint(item.markB, item.localPinB, item.b))}</td>
      <td class="wrap">${escapeHtml(item.note || "—")}</td>
    </tr>
  `).join("");

  table.className = "";
  table.innerHTML = `<div class="table-wrap"><table><thead><tr><th>#</th><th>Статус</th><th>Провод</th><th>Конец A</th><th>Конец B</th><th>Примечание</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  attachPairRowHandlers(table);
}

function renderMatrixConnections(view) {
  const target = $("matrixConnections");
  if (!target) return;

  const items = buildMatrixRows(view);
  if (!items.length) {
    target.className = "empty";
    target.textContent = "Связи для текущего вида не найдены.";
    return;
  }

  const rows = items
    .map((item) =>
      `<tr data-a="${escapeHtml(item.a)}" data-b="${escapeHtml(item.b)}" data-kind="${escapeHtml(item.kind)}">` +
      `<td>${statusChip(item.kind, item.label)}</td>` +
      `<td>${escapeHtml(contactLabel(item.a))}</td>` +
      `<td>${escapeHtml(contactLabel(item.b))}</td>` +
      `<td class="wrap">${escapeHtml(pairDisplayText(item))}${pairMetaText(item) ? `<br><small>${escapeHtml(pairMetaText(item))}</small>` : ""}</td>` +
      `</tr>`)
    .join("");

  target.className = "";
  target.innerHTML =
    `<div class="table-wrap"><table><thead><tr><th>Статус</th><th>Контакт A</th><th>Контакт B</th><th>Провод и маркировка</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  attachPairRowHandlers(target);
}

function renderMatrixAxes(size) {
  const top = $("matrixAxisTop");
  const left = $("matrixAxisLeft");
  top.innerHTML = "";
  left.innerHTML = "";

  const side = Math.sqrt(size);
  for (let index = 0; index < side; index += 1) {
    const topLabel = document.createElement("span");
    const leftLabel = document.createElement("span");
    const label = String(index + 1);
    const major = index % 4 === 0;

    topLabel.textContent = major ? label : "·";
    leftLabel.textContent = major ? label : "·";
    topLabel.className = major ? "major" : "muted";
    leftLabel.className = major ? "major" : "muted";

    top.append(topLabel);
    left.append(leftLabel);
  }
}

function renderMatrix(view) {
  currentMatrixView = view;
  selectedMatrixPair = selectedMatrixPair && Number.isInteger(selectedMatrixPair.a) && Number.isInteger(selectedMatrixPair.b)
    ? selectedMatrixPair
    : null;
  renderMatrixConnections(view);
  renderReferenceGuide(view);
}

async function loadReferenceView(fileName) {
  try {
    const view = await api(`/api/reference/view?file=${encodeURIComponent(fileName)}`);
    lastMatrixKind = "reference";
    lastReferenceLoaded = fileName;
    renderMatrix(view);
  } catch (error) {
    toast(error.message);
  }
}

async function loadResultView(fileName) {
  try {
    const view = await api(`/api/result/view?file=${encodeURIComponent(fileName)}`);
    lastMatrixKind = "result";
    renderMatrix(view);
  } catch (error) {
    toast(error.message);
  }
}

async function loadDevice() {
  try {
    const device = await api("/api/device");
    const badge = $("deviceBadge");
    badge.textContent = `${device.deviceModel || device.hardware} · ${device.ip} · ${device.link ? "LINK" : "NO LINK"} · FW ${device.firmwareVersion || "?"}`;
    badge.className = `badge ${device.link ? "ok" : "error"}`;
    renderTimeSyncStatus(device);
    renderStorageStatus(device);
    applyServiceProtection(device);
  } catch (error) {
    $("deviceBadge").textContent = "Нет связи";
    $("deviceBadge").className = "badge error";
    renderTimeSyncStatus(null);
    renderStorageStatus(null);
    applyServiceProtection(null);
  }
}

async function deleteReferenceFile(fileName) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  if (!confirm(`Удалить эталон ${fileName}?`)) return;
  try {
    await api(`/api/reference?file=${encodeURIComponent(fileName)}`, { method: "DELETE" });
    toast("Эталон удален.");
    await loadFiles();
  } catch (error) {
    toast(error.message);
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
    if (type === "reference" && item.valid) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = () => loadReferenceView(item.file);
      actions.append(open);
      const remove = document.createElement("button");
      remove.textContent = "Удалить";
      remove.hidden = !serviceUnlockEnabled;
      remove.onclick = () => deleteReferenceFile(item.file);
      actions.append(remove);
    }
    if (type === "calculation" && item.file.endsWith(".json")) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = async () => loadResult(item.file);
      actions.append(open);
    }

    if (type === "calculation") {
      const remove = document.createElement("button");
      remove.textContent = "Удалить";
      remove.hidden = !serviceUnlockEnabled;
      remove.onclick = () => deleteCalculationFile(item.file);
      actions.append(remove);
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
    const calculationParams = new URLSearchParams();
    if ($("calculationFilterDateFrom")?.value) calculationParams.set("dateFrom", $("calculationFilterDateFrom").value);
    if ($("calculationFilterDateTo")?.value) calculationParams.set("dateTo", $("calculationFilterDateTo").value);
    if ($("calculationFilterDeviceId")?.value.trim()) calculationParams.set("deviceId", $("calculationFilterDeviceId").value.trim());
    if ($("calculationFilterOperator")?.value.trim()) calculationParams.set("operator", $("calculationFilterOperator").value.trim());
    const calculationsUrl = calculationParams.size ? `/api/calculations?${calculationParams}` : "/api/calculations";

    const [references, calculationsResponse] = await Promise.all([
      api("/api/references"),
      api(calculationsUrl)
    ]);
    const calculations = Array.isArray(calculationsResponse) ? [...calculationsResponse] : [];

    if (!calculations.length && latestStatus?.lastResult) {
      calculations.push({
        file: latestStatus.lastResult,
        name: latestStatus.lastResult,
        size: 0,
        kind: "report"
      });
    }

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
    if (select.value && lastMatrixKind !== "result" && select.value !== lastReferenceLoaded) {
      loadReferenceView(select.value);
    }

    currentReferences = references;
    currentCalculations = calculations;
    primeSuggestionsFromRecords(currentReferences);
    primeSuggestionsFromRecords(currentCalculations);
    refreshInputSuggestions();
    renderJournal(currentCalculations);
    renderReferenceJournal(currentReferences);
  } catch (error) {
    toast(error.message);
  }
}

async function runSinglePinDiagnostic() {
  const source = Number($("diagnosticSourcePin").value);
  if (!Number.isInteger(source) || source < 0 || source > 1023) {
    toast("Укажите source pin от 0 до 1023.");
    return;
  }

  const resultEl = $("diagnosticResult");
  const button = $("diagnosticScanButton");
  resultEl.className = "message";
  resultEl.textContent = "Проверка выполняется...";
  button.disabled = true;

  try {
    const result = await api(`/api/diagnostic/pin?source=${source}`);
    const links = Array.isArray(result.connectedPins) ? result.connectedPins : [];
    const baseline = Array.isArray(result.baselineLowPins) ? result.baselineLowPins : [];

    if (!links.length) {
      resultEl.className = "empty";
      resultEl.innerHTML =
        `<strong>Источник:</strong> ${escapeHtml(contactLabel(result.sourcePin))}<br>` +
        `Связей не найдено.<br>` +
        `Сам источник прочитан как LOW: ${result.sourceSeenLow ? "да" : "нет"}<br>` +
        `Фоновых LOW до старта: ${baseline.length}`;
      return;
    }

    const rows = links
      .map((item) => `<tr><td>${escapeHtml(contactLabel(item.pin))}</td></tr>`)
      .join("");

    resultEl.className = "";
    resultEl.innerHTML =
      `<p><strong>Источник:</strong> ${escapeHtml(contactLabel(result.sourcePin))}<br>` +
      `Найдено связей: ${escapeHtml(result.connectedCount)}<br>` +
      `Сам источник прочитан как LOW: ${result.sourceSeenLow ? "да" : "нет"}<br>` +
      `Фоновых LOW до старта: ${baseline.length}</p>` +
      `<div class="table-wrap"><table><thead><tr><th>Найденные контакты</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  } catch (error) {
    resultEl.className = "message";
    resultEl.textContent = error.message;
    toast(error.message);
  } finally {
    button.disabled = false;
  }
}

function journalStatus(item) {
  if ((item.missingLinks || 0) > 0) return { kind: "missing", label: "Обрывы" };
  if ((item.extraLinks || 0) > 0) return { kind: "extra", label: "Паразитные" };
  if ((item.asymmetricLinks || 0) > 0) return { kind: "asymmetric", label: "Асимметрия" };
  return { kind: "ok", label: "Норма" };
}

function journalStatusRank(item) {
  const status = journalStatus(item).kind;
  if (status === "missing") return 3;
  if (status === "extra") return 2;
  if (status === "asymmetric") return 1;
  return 0;
}

function journalDateValue(item) {
  const raw = item?.createdAtLocal || "";
  const value = Date.parse(raw);
  return Number.isNaN(value) ? 0 : value;
}

function sortJournalReports(files) {
  const sortMode = $("journalSort")?.value || "date_desc";
  const reports = [...files];

  reports.sort((left, right) => {
    if (sortMode === "date_asc") return journalDateValue(left) - journalDateValue(right);
    if (sortMode === "result_bad_first") {
      const statusDiff = journalStatusRank(right) - journalStatusRank(left);
      return statusDiff || (journalDateValue(right) - journalDateValue(left));
    }
    if (sortMode === "result_ok_first") {
      const statusDiff = journalStatusRank(left) - journalStatusRank(right);
      return statusDiff || (journalDateValue(right) - journalDateValue(left));
    }
    return journalDateValue(right) - journalDateValue(left);
  });

  return reports;
}

function sortReferenceReports(files) {
  const sortMode = $("referenceJournalSort")?.value || "date_desc";
  const reports = [...files];

  reports.sort((left, right) => {
    if (sortMode === "date_asc") return journalDateValue(left) - journalDateValue(right);
    if (sortMode === "name_asc") return String(left.name || left.file).localeCompare(String(right.name || right.file), "ru");
    if (sortMode === "name_desc") return String(right.name || right.file).localeCompare(String(left.name || left.file), "ru");
    return journalDateValue(right) - journalDateValue(left);
  });

  return reports;
}

function exportCalculationsCsv() {
  const rows = [["Дата", "Итог", "Эталон", "Сборка", "Прибор", "Проверяющий", "Обрывы", "Паразитные", "Асимметрия", "Файл"]];
  for (const item of sortJournalReports(currentCalculations.filter((entry) => entry?.file?.endsWith(".json")))) {
    rows.push([
      formatStoredDate(item.createdAtLocal || ""),
      journalStatus(item).label,
      item.name || item.file,
      item.cableType || "",
      item.deviceId || "",
      item.operator || "",
      item.missingLinks || 0,
      item.extraLinks || 0,
      item.asymmetricLinks || 0,
      item.file || ""
    ]);
  }
  downloadTextFile("calculations-journal.csv", rows.map((row) => row.map(csvEscape).join(";")).join("\n"), "text/csv;charset=utf-8");
}

function exportReferencesCsv() {
  const rows = [["Дата", "Название", "Сборка", "Ревизия", "Прибор", "Оператор", "Статус", "Файл"]];
  for (const item of sortReferenceReports(currentReferences.filter((entry) => entry?.file?.endsWith(".ref")))) {
    rows.push([
      formatStoredDate(item.createdAtLocal || ""),
      item.name || item.file,
      item.cableType || "",
      item.revision || "",
      item.deviceId || "",
      item.operator || "",
      approvalStatusLabel(item.approvalStatus),
      item.file || ""
    ]);
  }
  downloadTextFile("references-journal.csv", rows.map((row) => row.map(csvEscape).join(";")).join("\n"), "text/csv;charset=utf-8");
}

async function exportCalculationReportCsv(fileName) {
  try {
    const result = await api(`/api/result?file=${encodeURIComponent(fileName)}`);
    const summary = result.summary || {};
    const time = result.time || {};
    const reference = result.referenceMetadata || {};
    const measurement = result.measurementMetadata || {};
    const rows = [
      ["Раздел", "Поле", "Значение"],
      ["Отчет", "Файл", fileName],
      ["Отчет", "Итог", result.passed ? "Норма" : "Ошибки"],
      ["Отчет", "Дата проверки", formatStoredDate(time.startedAtLocal || "")],
      ["Отчет", "Часовой пояс", time.timeZone || ""],
      ["Отчет", "Длительность, мс", String(result.elapsedMs || 0)],
      ["Проверка", "Тип сборки", measurement.cableType || ""],
      ["Проверка", "Прибор", measurement.deviceId || ""],
      ["Проверка", "Оператор", measurement.operator || ""],
      ["Эталон", "Имя", reference.name || result.reference || ""],
      ["Эталон", "Тип сборки", reference.cableType || ""],
      ["Эталон", "Ревизия", reference.revision || ""],
      ["Эталон", "Прибор", reference.deviceId || ""],
      ["Эталон", "Оператор", reference.operator || ""],
      ["Эталон", "Статус", approvalStatusLabel(reference.approvalStatus)],
      ["Сводка", "Ожидалось связей", String(summary.expectedLinks || 0)],
      ["Сводка", "Измерено связей", String(summary.measuredLinks || 0)],
      ["Сводка", "Обрывы", String(summary.missingLinks || 0)],
      ["Сводка", "Паразитные", String(summary.extraLinks || 0)],
      ["Сводка", "Асимметрия", String(summary.asymmetricLinks || 0)],
      [],
      ["Тип", "Контакт A", "Контакт B", "Задержка"]
    ];

    const appendPairs = (title, pairs) => {
      for (const pair of Array.isArray(pairs) ? pairs : []) {
        rows.push([
          title,
          contactLabel(pair.source ?? pair.a),
          contactLabel(pair.target ?? pair.b),
          String(pair.delayUs ?? pair.delayMs ?? "")
        ]);
      }
    };

    appendPairs("Обрыв", result.missing);
    appendPairs("Паразитная связь", result.extra);
    appendPairs("Асимметрия", result.asymmetric);

    const exportName = String(fileName || "report.json").replace(/\.json$/i, ".csv");
    downloadTextFile(exportName, rows.map((row) => row.map(csvEscape).join(";")).join("\n"), "text/csv;charset=utf-8");
  } catch (error) {
    toast(error.message);
  }
}

function renderJournal(files) {
  const target = $("journalTable");
  const summary = $("journalSummary");
  const pagination = $("journalPagination");
  if (!target || !summary || !pagination) return;

  const reports = sortJournalReports((Array.isArray(files) ? files : []).filter((item) => item?.file?.endsWith(".json")));
  summary.textContent = `${reports.length} записей`;

  if (!reports.length) {
    target.className = "empty";
    target.textContent = "Записей пока нет.";
    pagination.hidden = true;
    pagination.innerHTML = "";
    return;
  }

  const totalPages = Math.max(1, Math.ceil(reports.length / JOURNAL_PAGE_SIZE));
  journalPage = Math.min(Math.max(journalPage, 1), totalPages);
  const start = (journalPage - 1) * JOURNAL_PAGE_SIZE;
  const pageReports = reports.slice(start, start + JOURNAL_PAGE_SIZE);

  const rows = pageReports.map((item) => {
    const status = journalStatus(item);
    return `<tr>
      <td>${escapeHtml(formatStoredDate(item.createdAtLocal || ""))}</td>
      <td>${statusChip(status.kind, status.label)}</td>
      <td class="wrap">${escapeHtml(item.name || item.file)}</td>
      <td class="wrap">${escapeHtml(item.cableType || "не указан")}</td>
      <td>${escapeHtml(item.deviceId || "не указан")}</td>
      <td class="wrap">${escapeHtml(item.operator || "не указан")}</td>
      <td>${escapeHtml(String(item.missingLinks || 0))}</td>
      <td>${escapeHtml(String(item.extraLinks || 0))}</td>
      <td>${escapeHtml(String(item.asymmetricLinks || 0))}</td>
      <td><div class="journal-actions"><button type="button" data-open-report="${escapeHtml(item.file)}">Открыть</button><button type="button" data-export-report-csv="${escapeHtml(item.file)}">Экспорт CSV</button><button type="button" data-delete-report="${escapeHtml(item.file)}" ${serviceUnlockEnabled ? "" : "hidden"}>Удалить</button><a href="/api/download?type=calculation&file=${encodeURIComponent(item.file)}">Выгрузить</a></div></td>
    </tr>`;
  }).join("");

  target.className = "";
  target.innerHTML = `<div class="table-wrap"><table class="journal-table"><thead><tr><th>Дата</th><th>Итог</th><th class="wrap">Эталон</th><th class="wrap">Сборка</th><th>Прибор</th><th class="wrap">Проверяющий</th><th>Обрывы</th><th>Паразитные</th><th>Асимметрия</th><th>Действие</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  pagination.hidden = false;
  pagination.innerHTML = `
    <button type="button" data-journal-page="prev" ${journalPage <= 1 ? "disabled" : ""}>Назад</button>
    <span class="journal-pagination-info">Страница ${journalPage} из ${totalPages}</span>
    <button type="button" data-journal-page="next" ${journalPage >= totalPages ? "disabled" : ""}>Вперёд</button>
  `;

  target.querySelectorAll("[data-open-report]").forEach((button) => {
    button.addEventListener("click", async () => {
      await loadResult(button.dataset.openReport);
    });
  });
  target.querySelectorAll("[data-export-report-csv]").forEach((button) => {
    button.addEventListener("click", async () => {
      await exportCalculationReportCsv(button.dataset.exportReportCsv);
    });
  });
  target.querySelectorAll("[data-delete-report]").forEach((button) => {
    button.addEventListener("click", async () => {
      await deleteCalculationFile(button.dataset.deleteReport);
    });
  });
  pagination.querySelectorAll("[data-journal-page]").forEach((button) => {
    button.addEventListener("click", () => {
      journalPage += button.dataset.journalPage === "next" ? 1 : -1;
      renderJournal(files);
    });
  });
}

function renderReferenceJournal(files) {
  const target = $("referenceJournalTable");
  const summary = $("referenceJournalSummary");
  const pagination = $("referenceJournalPagination");
  if (!target || !summary || !pagination) return;

  const reports = sortReferenceReports((Array.isArray(files) ? files : []).filter((item) => item?.file?.endsWith(".ref")));
  summary.textContent = `${reports.length} записей`;

  if (!reports.length) {
    target.className = "empty";
    target.textContent = "Записей пока нет.";
    pagination.hidden = true;
    pagination.innerHTML = "";
    return;
  }

  const totalPages = Math.max(1, Math.ceil(reports.length / JOURNAL_PAGE_SIZE));
  referenceJournalPage = Math.min(Math.max(referenceJournalPage, 1), totalPages);
  const start = (referenceJournalPage - 1) * JOURNAL_PAGE_SIZE;
  const pageReports = reports.slice(start, start + JOURNAL_PAGE_SIZE);

  const rows = pageReports.map((item) => `
    <tr>
      <td>${escapeHtml(formatStoredDate(item.createdAtLocal || ""))}</td>
      <td class="wrap">${escapeHtml(item.name || item.file)}</td>
      <td class="wrap">${escapeHtml(item.cableType || "не указан")}</td>
      <td>${escapeHtml(item.revision || "не указана")}</td>
      <td>${escapeHtml(item.deviceId || "не указан")}</td>
      <td class="wrap">${escapeHtml(item.operator || "не указан")}</td>
      <td>${escapeHtml(approvalStatusLabel(item.approvalStatus))}</td>
      <td><div class="journal-actions"><button type="button" data-open-reference="${escapeHtml(item.file)}">Открыть</button><button type="button" data-delete-reference="${escapeHtml(item.file)}" ${serviceUnlockEnabled ? "" : "hidden"}>Удалить</button><a href="/api/download?type=reference&file=${encodeURIComponent(item.file)}">Выгрузить</a></div></td>
    </tr>
  `).join("");

  target.className = "";
  target.innerHTML = `<div class="table-wrap"><table class="journal-table"><thead><tr><th>Дата</th><th class="wrap">Название</th><th class="wrap">Сборка</th><th>Ревизия</th><th>Прибор</th><th class="wrap">Оператор</th><th>Статус</th><th>Действие</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  pagination.hidden = false;
  pagination.innerHTML = `
    <button type="button" data-reference-page="prev" ${referenceJournalPage <= 1 ? "disabled" : ""}>Назад</button>
    <span class="journal-pagination-info">Страница ${referenceJournalPage} из ${totalPages}</span>
    <button type="button" data-reference-page="next" ${referenceJournalPage >= totalPages ? "disabled" : ""}>Вперёд</button>
  `;

  target.querySelectorAll("[data-open-reference]").forEach((button) => {
    button.addEventListener("click", async () => {
      if ($("referenceSelect")) $("referenceSelect").value = button.dataset.openReference;
      await loadReferenceView(button.dataset.openReference);
    });
  });
  target.querySelectorAll("[data-delete-reference]").forEach((button) => {
    button.addEventListener("click", async () => {
      await deleteReferenceFile(button.dataset.deleteReference);
    });
  });
  pagination.querySelectorAll("[data-reference-page]").forEach((button) => {
    button.addEventListener("click", () => {
      referenceJournalPage += button.dataset.referencePage === "next" ? 1 : -1;
      renderReferenceJournal(files);
    });
  });
}

function uploadFile(kind, input, bar, text) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
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

async function syncBrowserTime() {
  const button = $("syncTimeButton");
  const previousText = button.textContent;
  button.disabled = true;
  button.textContent = "Синхронизация…";
  try {
    const params = browserTimeParams();
    const result = await api(`/api/time/sync?${params}`, { method: "POST" });
    renderTimeSyncStatus({
      clockSynced: true,
      clockLastSyncLocal: result.local,
      clockLastSyncUtc: result.utc,
      clockTimeZone: result.timeZone
    });
    toast("Время платы синхронизировано с браузером.");
    await loadDevice();
  } catch (error) {
    toast(error.message);
  } finally {
    button.disabled = false;
    button.textContent = previousText;
  }
}

$("captureButton").addEventListener("click", async () => {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  try {
    const params = referenceMetadataParams();
    localStorage.setItem("cableTesterDeviceId", $("deviceId").value.trim());
    localStorage.setItem("cableTesterOperator", $("operatorName").value.trim());
    rememberCurrentMetadata();
    await api(`/api/reference/capture?${params}`, { method: "POST" });
    setBusy(true);
    toast("Эталон v3 запущен. Дата и время получены из браузера.");
  } catch (error) {
    toast(error.message);
  }
});

$("testButton").addEventListener("click", async () => {
  const reference = $("referenceSelect").value;
  if (!reference) return toast("Выберите совместимый эталон v3.");
  try {
    const params = browserTimeParams();
    params.set("reference", reference);
    await api(`/api/test/start?${params}`, { method: "POST" });
    setBusy(true);
    toast("Проверка кабеля запущена. Время получено из браузера.");
  } catch (error) {
    toast(error.message);
  }
});

$("diagnosticScanButton").addEventListener("click", runSinglePinDiagnostic);

$("syncTimeButton").addEventListener("click", syncBrowserTime);

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
ensurePrintButton();
if ($("printResultButton")) {
  $("printResultButton").addEventListener("click", printCurrentResult);
}
$("refreshResultsButton").addEventListener("click", async () => {
  const calculationParams = new URLSearchParams();
  if ($("calculationFilterDateFrom")?.value) calculationParams.set("dateFrom", $("calculationFilterDateFrom").value);
  if ($("calculationFilterDateTo")?.value) calculationParams.set("dateTo", $("calculationFilterDateTo").value);
  if ($("calculationFilterDeviceId")?.value.trim()) calculationParams.set("deviceId", $("calculationFilterDeviceId").value.trim());
  if ($("calculationFilterOperator")?.value.trim()) calculationParams.set("operator", $("calculationFilterOperator").value.trim());
  const files = await api(calculationParams.size ? `/api/calculations?${calculationParams}` : "/api/calculations");
  const reports = files.filter((item) => item.file.endsWith(".json"));
  if (reports.length) loadResult(reports[reports.length - 1].file);
});

for (const id of ["calculationFilterDateFrom", "calculationFilterDateTo", "calculationFilterDeviceId", "calculationFilterOperator"]) {
  if ($(id)) $(id).addEventListener("change", () => {
    journalPage = 1;
    loadFiles();
  });
  if ($(id) && $(id).tagName === "INPUT" && $(id).type === "text") $(id).addEventListener("input", () => {
    journalPage = 1;
    loadFiles();
  });
}

setupCollapsiblePanel("toggleReferenceFormButton", "referenceFormPanel");
setupCollapsiblePanel("toggleUploadPanelButton", "uploadPanel");
setupCollapsiblePanel("toggleReferenceGuideButton", "referenceGuidePanel");
setupStorageUi();

if ($("journalSort")) {
  $("journalSort").addEventListener("change", () => {
    journalPage = 1;
    loadFiles();
  });
}
if ($("referenceJournalSort")) {
  $("referenceJournalSort").addEventListener("change", () => {
    referenceJournalPage = 1;
    renderReferenceJournal(currentReferences);
  });
}
if ($("calculationCsvButton")) {
  $("calculationCsvButton").addEventListener("click", exportCalculationsCsv);
}
if ($("referenceCsvButton")) {
  $("referenceCsvButton").addEventListener("click", exportReferencesCsv);
}

$("referenceSelect").addEventListener("change", () => {
  if ($("referenceSelect").value) loadReferenceView($("referenceSelect").value);
});

$("deviceId").value = localStorage.getItem("cableTesterDeviceId") || "";
$("operatorName").value = localStorage.getItem("cableTesterOperator") || "";
refreshInputSuggestions();
for (const id of ["deviceId", "operatorName", "testDeviceId", "testOperatorName"]) {
  if ($(id)) $(id).addEventListener("change", rememberCurrentMetadata);
}

async function deleteCalculationFile(fileName) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  if (!confirm(`Удалить расчет ${fileName}?`)) return;
  try {
    await api(`/api/calculation?file=${encodeURIComponent(fileName)}`, { method: "DELETE" });
    toast("Расчет удален.");
    await loadFiles();
    await loadDevice();
  } catch (error) {
    toast(error.message);
  }
}

function renderStorageStatus(device) {
  const target = $("storageStatus");
  if (!target) return;

  const total = Number(device?.storageTotalBytes || 0);
  const used = Number(device?.storageUsedBytes || 0);
  const free = Math.max(Number(device?.storageFreeBytes || 0), 0);
  const percent = total > 0 ? Math.min(100, Math.round((used * 100) / total)) : 0;
  target.innerHTML = `<strong>FFat</strong> · занято ${fileSize(used)} из ${fileSize(total)} (${percent}%) · свободно ${fileSize(free)}`;
}

async function loadDevice() {
  try {
    const device = await api("/api/device");
    const badge = $("deviceBadge");
    badge.textContent = `${device.deviceModel || device.hardware} · ${device.ip} · ${device.link ? "LINK" : "NO LINK"} · FW ${device.firmwareVersion || "?"}`;
    badge.className = `badge ${device.link ? "ok" : "error"}`;
    renderTimeSyncStatus(device);
    renderStorageStatus(device);
    applyServiceProtection(device);
  } catch (error) {
    $("deviceBadge").textContent = "Нет связи";
    $("deviceBadge").className = "badge error";
    renderTimeSyncStatus(null);
    renderStorageStatus(null);
    applyServiceProtection(null);
  }
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
        : `Несовместимый или поврежденный эталон · ${escapeHtml(fileSize(item.size))}`;
    } else {
      const resultLabel = item.passed === true
        ? "норма"
        : item.passed === false
          ? "ошибки"
          : (item.kind || "report");
      details.innerHTML =
        `${escapeHtml(formatStoredDate(item.createdAtLocal || ""))} · ${escapeHtml(resultLabel)}<br>` +
        `${escapeHtml(item.deviceId || "прибор не указан")} · ${escapeHtml(item.operator || "оператор не указан")}<br>` +
        `${escapeHtml(fileSize(item.size))}`;
    }

    info.append(name, document.createElement("br"), details);

    const actions = document.createElement("div");
    actions.className = "file-actions";
    if (type === "reference" && item.valid) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = () => loadReferenceView(item.file);
      actions.append(open);

      const remove = document.createElement("button");
      remove.textContent = "Удалить";
      remove.onclick = () => deleteReferenceFile(item.file);
      actions.append(remove);
    }
    if (type === "calculation" && item.file.endsWith(".json")) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = async () => loadResult(item.file);
      actions.append(open);
    }
    if (type === "calculation") {
      const remove = document.createElement("button");
      remove.textContent = "Удалить";
      remove.onclick = () => deleteCalculationFile(item.file);
      actions.append(remove);
    }

    const link = document.createElement("a");
    link.textContent = "Выгрузить";
    link.href = `/api/download?type=${encodeURIComponent(type)}&file=${encodeURIComponent(item.file)}`;
    actions.append(link);

    row.append(info, actions);
    element.append(row);
  }
}

function renderPairs(target, pairs, truncated) {
  if (!pairs?.length) {
    target.innerHTML = '<div class="empty">Нет.</div>';
    return;
  }

  const kind = target.id === "missingTable"
    ? "missing"
    : target.id === "extraTable"
      ? "extra"
      : target.id === "asymmetricTable"
        ? "asymmetric"
        : "ok";
  const label = kind === "missing"
    ? "Обрыв"
    : kind === "extra"
      ? "Паразитная связь"
      : kind === "asymmetric"
        ? "Асимметрия"
        : "Норма";

  const rows = pairs
    .map((pair) =>
      `<tr data-a="${escapeHtml(pair.a)}" data-b="${escapeHtml(pair.b)}" data-kind="${escapeHtml(kind)}">` +
      `<td>${statusChip(kind, label)}</td>` +
      `<td>${escapeHtml(contactLabel(pair.a))}</td>` +
      `<td>${escapeHtml(contactLabel(pair.b))}</td>` +
      `<td class="wrap">${escapeHtml(pairDisplayText(pair))}${pairMetaText(pair) ? `<br><small>${escapeHtml(pairMetaText(pair))}</small>` : ""}</td>` +
      `</tr>`)
    .join("");

  target.innerHTML =
    `<div class="table-wrap"><table><thead><tr><th>Статус</th><th>Контакт A</th><th>Контакт B</th><th>Провод и маркировка</th></tr></thead><tbody>${rows}</tbody></table></div>` +
    `${truncated ? "<p>Список сокращен. Полное число указано в сводке.</p>" : ""}`;
  attachPairRowHandlers(target);
}

function comparisonMetadataParams() {
  const required = [
    ["testCableType", "Введите тип сборки расчета."],
    ["testDeviceId", "Укажите прибор проверки."],
    ["testOperatorName", "Укажите оператора проверки."]
  ];
  for (const [id, message] of required) {
    if (!$(id) || !$(id).value.trim()) throw new Error(message);
  }

  const params = browserTimeParams();
  params.set("cableType", $("testCableType").value.trim());
  params.set("deviceId", $("testDeviceId").value.trim());
  params.set("operator", $("testOperatorName").value.trim());
  return params;
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
        : `Несовместимый или поврежденный эталон · ${escapeHtml(fileSize(item.size))}`;
    } else {
      details.textContent = `${fileSize(item.size)}${item.kind ? ` · ${item.kind}` : ""}`;
    }

    info.append(name, document.createElement("br"), details);

    const actions = document.createElement("div");
    actions.className = "file-actions";
    if (type === "reference" && item.valid) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = () => loadReferenceView(item.file);
      actions.append(open);

      const remove = document.createElement("button");
      remove.textContent = "Удалить";
      remove.onclick = () => deleteReferenceFile(item.file);
      actions.append(remove);
    }
    if (type === "calculation" && (item.file.endsWith(".json") || item.kind === "report")) {
      const open = document.createElement("button");
      open.textContent = "Открыть";
      open.onclick = async () => loadResult(item.file);
      actions.append(open);
    }
    if (type === "calculation") {
      const remove = document.createElement("button");
      remove.textContent = "Удалить";
      remove.onclick = () => deleteCalculationFile(item.file);
      actions.append(remove);
    }

    const link = document.createElement("a");
    link.textContent = "Выгрузить";
    link.href = `/api/download?type=${encodeURIComponent(type)}&file=${encodeURIComponent(item.file)}`;
    actions.append(link);

    row.append(info, actions);
    element.append(row);
  }
}

async function loadResult(fileName) {
  try {
    const result = await api(`/api/result?file=${encodeURIComponent(fileName)}`);
    lastResultLoaded = fileName;
    if ($("printResultButton")) $("printResultButton").disabled = false;
    const summary = result.summary || {};
    const storedTime = result.time || {};
    const reference = result.referenceMetadata || {};
    const measurement = result.measurementMetadata || {};

    $("resultSummary").className = result.passed ? "result-ok" : "result-error";
    $("resultSummary").innerHTML = `
      <strong>${result.passed ? "КАБЕЛЬ ИСПРАВЕН" : "ОБНАРУЖЕНЫ ОТЛИЧИЯ"}</strong><br>
      Проверка: ${escapeHtml(formatStoredDate(storedTime.startedAtLocal))} · ${escapeHtml(storedTime.timeZone || "часовой пояс не указан")}<br>
      Расчет: ${escapeHtml(measurement.cableType || "не указан")} · прибор ${escapeHtml(measurement.deviceId || "не указан")} · оператор ${escapeHtml(measurement.operator || "не указан")}<br>
      Эталон: ${escapeHtml(reference.name || result.reference || "не указан")} · ${escapeHtml(reference.cableType || "")} · рев. ${escapeHtml(reference.revision || "")}<br>
      Статус эталона: ${escapeHtml(approvalStatusLabel(reference.approvalStatus))}; прибор: ${escapeHtml(reference.deviceId || "не указан")}; оператор эталона: ${escapeHtml(reference.operator || "не указан")}<br>
      CRC таблицы: ${escapeHtml(formatCrc32(reference.mappingCrc32))}; CRC эталонной матрицы: ${escapeHtml(formatCrc32(reference.matrixCrc32))}<br>
      Обрывы: ${summary.missingLinks || 0}; паразитные связи: ${summary.extraLinks || 0}; асимметрии: ${summary.asymmetricLinks || 0}; длительность: ${formatSeconds(result.elapsedMs || 0)}.
    `;
    $("errorTables").hidden = false;
    await loadResultView(fileName);
    let missingPairs = currentMatrixView?.missingConnections || result.missing;
    let extraPairs = currentMatrixView?.extraConnections || result.extra;
    let asymmetricPairs = currentMatrixView?.asymmetricConnections || result.asymmetric;

    if (!pairsHaveAnnotations(missingPairs) && result.reference) {
      try {
        const referenceView = await api(`/api/reference/view?file=${encodeURIComponent(result.reference)}`);
        const annotationMap = buildAnnotationMap(referenceView);
        const contactAnnotationMap = buildContactAnnotationMap(referenceView);
        missingPairs = enrichPairsWithAnnotations(missingPairs, annotationMap, contactAnnotationMap);
        extraPairs = enrichPairsWithAnnotations(extraPairs, annotationMap, contactAnnotationMap);
        asymmetricPairs = enrichPairsWithAnnotations(asymmetricPairs, annotationMap, contactAnnotationMap);
      } catch (annotationError) {
        console.warn("Could not enrich result pairs from reference annotations", annotationError);
      }
    }

    renderPairs($("missingTable"), missingPairs, result.missingTruncated);
    renderPairs($("extraTable"), extraPairs, result.extraTruncated);
    renderPairs($("asymmetricTable"), asymmetricPairs, result.asymmetricTruncated);
    currentResultForPrint = {
      ...result,
      printPairs: {
        missing: missingPairs,
        extra: extraPairs,
        asymmetric: asymmetricPairs
      }
    };
    revealOpenedResult(fileName);
  } catch (error) {
    toast(error.message);
  }
}

if ($("testCableType")) $("testCableType").value = localStorage.getItem("cableTesterTestCableType") || "";
if ($("testDeviceId")) $("testDeviceId").value = localStorage.getItem("cableTesterTestDeviceId") || $("deviceId").value || "";
if ($("testOperatorName")) $("testOperatorName").value = localStorage.getItem("cableTesterTestOperator") || $("operatorName").value || "";

if ($("testButton")) {
  const replacement = $("testButton").cloneNode(true);
  $("testButton").replaceWith(replacement);
  replacement.addEventListener("click", async () => {
    const reference = $("referenceSelect").value;
    if (!reference) return toast("Выберите совместимый эталон v3.");
    try {
      const params = comparisonMetadataParams();
      params.set("reference", reference);
      rememberCurrentMetadata();
      await api(`/api/test/start?${params}`, { method: "POST" });
      setBusy(true);
      localStorage.setItem("cableTesterTestCableType", $("testCableType").value.trim());
      localStorage.setItem("cableTesterTestDeviceId", $("testDeviceId").value.trim());
      localStorage.setItem("cableTesterTestOperator", $("testOperatorName").value.trim());
      toast("Проверка кабеля запущена. Время получено из браузера.");
    } catch (error) {
      toast(error.message);
    }
  });
}

$("toggleNamedConnectionsButton")?.addEventListener("click", () => {
  namedConnectionsExpanded = !namedConnectionsExpanded;
  updateNamedConnectionsToggle();
});

updateNamedConnectionsToggle();
loadInitialStatus();
connectEvents();
loadDevice();
loadFiles();
setInterval(loadDevice, 10000);

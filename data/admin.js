const $ = (id) => document.getElementById(id);

let serviceUnlockEnabled = false;
let referencePage = 1;
let currentReferences = [];
let latestDevice = null;
let manualConnections = [];
let selectedManualPins = [];
let activeManualPairKey = "";
let hoveredManualPin = null;
let editingReferenceFile = "";
let editingReferenceName = "";

const REFERENCE_PAGE_SIZE = 20;
const HISTORY_LIMIT = 20;
const DEVICE_HISTORY_KEY = "cableTesterDeviceHistory";
const OPERATOR_HISTORY_KEY = "cableTesterOperatorHistory";
const PIN_COUNT = 1024;
const MATRIX_SIDE = 32;
const ROW_BYTES = PIN_COUNT / 8;
const MATRIX_BYTES = PIN_COUNT * ROW_BYTES;
const REFERENCE_MAGIC = 0x31464243;
const FILE_FORMAT_VERSION = 3;
const PACKED_BLOB_MAGIC = 0x314B4350;
const REFERENCE_HEADER_BYTES = 756;

function toast(message) {
  const el = $("toast");
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => {
    el.hidden = true;
  }, 4200);
}

function api(url, options = {}) {
  return fetch(url, { cache: "no-store", ...options })
    .then(async (response) => {
      const text = await response.text();
      let data;
      try {
        data = JSON.parse(text);
      } catch {
        data = text;
      }
      if (!response.ok) throw new Error(data?.error || text || `HTTP ${response.status}`);
      return data;
    });
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function formatStoredDate(value) {
  if (!value) return "не указана";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString("ru-RU");
}

function fileSize(bytes) {
  if (bytes < 1024) return `${bytes} Б`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} КиБ`;
  return `${(bytes / 1024 / 1024).toFixed(1)} МиБ`;
}

function contactLabel(pinZeroBased) {
  return `Контакт ${Number(pinZeroBased) + 1}`;
}

function parseContactValue(value) {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 1 || numeric > PIN_COUNT) return null;
  return numeric - 1;
}

function pairKey(a, b) {
  const left = Math.min(a, b);
  const right = Math.max(a, b);
  return `${left}:${right}`;
}

function approvalStatusLabel(value) {
  if (value === "approved") return "Утверждён";
  if (value === "pending") return "На утверждении";
  if (value === "rejected") return "Отклонён";
  if (value === "archived") return "Архивный";
  return "Черновик";
}

function serviceUnlockMessage() {
  return "Сервисный тумблер выключен. Включите его для создания, загрузки и удаления.";
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
  rememberHistoryValue(OPERATOR_HISTORY_KEY, $("operatorName")?.value);
  refreshInputSuggestions();
}

function primeSuggestionsFromReferences(records) {
  for (const item of Array.isArray(records) ? records : []) {
    rememberHistoryValue(DEVICE_HISTORY_KEY, item?.deviceId);
    rememberHistoryValue(OPERATOR_HISTORY_KEY, item?.operator);
  }
}

function manualPairIndex(a, b) {
  const key = pairKey(a, b);
  return manualConnections.findIndex((pair) => pair.key === key);
}

function renderManualStatus() {
  const status = $("manualEditorStatus");
  if (!status) return;

  if (!manualConnections.length) {
    status.textContent = selectedManualPins.length === 1
      ? `Выбран ${contactLabel(selectedManualPins[0])}. Выберите второй контакт для добавления связи.`
      : "Пока связей нет. Выберите два контакта, чтобы добавить первую пару.";
    return;
  }

  const selectedText = selectedManualPins.length === 1
    ? ` Сейчас выбран ${contactLabel(selectedManualPins[0])}.`
    : "";
  status.textContent = `Добавлено связей: ${manualConnections.length}.${selectedText}`;
}

function renderEditingState() {
  const target = $("manualEditingState");
  if (!target) return;
  target.textContent = editingReferenceFile
    ? `Открыт эталон для редактирования: ${editingReferenceName || editingReferenceFile}. При сохранении можно перезаписать его или создать новый файл.`
    : "Сейчас открыт режим создания нового ручного эталона.";
}

function renderManualPairList() {
  const target = $("manualPairList");
  if (!target) return;

  if (!manualConnections.length) {
    target.className = "empty";
    target.textContent = "Список связей пока пуст.";
    return;
  }

  target.className = "manual-pair-list";
  target.innerHTML = manualConnections.map((pair, index) => `
    <div class="manual-pair-item ${pair.key === activeManualPairKey ? "active" : ""}" data-manual-pair="${pair.key}">
      <div><strong>Связь ${index + 1}</strong><br>${escapeHtml(contactLabel(pair.a))} ↔ ${escapeHtml(contactLabel(pair.b))}</div>
      <button type="button" data-delete-manual-pair="${pair.key}">Удалить</button>
    </div>
  `).join("");

  target.querySelectorAll("[data-manual-pair]").forEach((item) => {
    item.addEventListener("click", (event) => {
      if (event.target instanceof HTMLElement && event.target.closest("[data-delete-manual-pair]")) return;
      activeManualPairKey = item.dataset.manualPair || "";
      renderManualPairList();
      renderManualMatrixCanvas();
    });
  });

  target.querySelectorAll("[data-delete-manual-pair]").forEach((button) => {
    button.addEventListener("click", (event) => {
      event.stopPropagation();
      manualConnections = manualConnections.filter((pair) => pair.key !== button.dataset.deleteManualPair);
      if (activeManualPairKey === button.dataset.deleteManualPair) activeManualPairKey = "";
      renderManualEditor();
    });
  });
}

function renderManualMatrixCanvas() {
  const canvas = $("manualMatrixCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const scale = canvas.width / MATRIX_SIDE;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  const usedPins = new Set();
  for (const pair of manualConnections) {
    usedPins.add(pair.a);
    usedPins.add(pair.b);
  }

  let activePins = [];
  if (activeManualPairKey) {
    const activePair = manualConnections.find((pair) => pair.key === activeManualPairKey);
    if (activePair) activePins = [activePair.a, activePair.b];
  }

  for (let pin = 0; pin < PIN_COUNT; ++pin) {
    const row = Math.floor(pin / MATRIX_SIDE);
    const col = pin % MATRIX_SIDE;
    const x = col * scale;
    const y = row * scale;

    let fill = "#d9e1ed";
    if (usedPins.has(pin)) fill = "#1f7a3f";
    if (activePins.includes(pin)) fill = "#1f6fd6";
    if (selectedManualPins.includes(pin)) fill = "#d98d00";
    if (hoveredManualPin === pin) fill = "#7f8fa6";

    ctx.fillStyle = fill;
    ctx.fillRect(x + 1, y + 1, scale - 2, scale - 2);
  }

  ctx.strokeStyle = "rgba(28, 39, 58, 0.14)";
  ctx.lineWidth = 1;
  for (let i = 0; i <= MATRIX_SIDE; ++i) {
    const offset = i * scale;
    ctx.beginPath();
    ctx.moveTo(offset, 0);
    ctx.lineTo(offset, canvas.height);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(0, offset);
    ctx.lineTo(canvas.width, offset);
    ctx.stroke();
  }
}

function updateManualHoverInfo(pin) {
  const target = $("manualHoverInfo");
  if (!target) return;
  if (!Number.isInteger(pin) || pin < 0 || pin >= PIN_COUNT) {
    target.textContent = "Наведите курсор на ячейку, чтобы увидеть номер контакта.";
    return;
  }

  const pairCount = manualConnections.filter((pair) => pair.a === pin || pair.b === pin).length;
  target.textContent = pairCount > 0
    ? `${contactLabel(pin)} · участвует в ${pairCount} ${pairCount === 1 ? "связи" : pairCount < 5 ? "связях" : "связях"}`
    : `${contactLabel(pin)} · пока не используется`;
}

function renderManualEditor() {
  renderManualStatus();
  renderEditingState();
  renderManualPairList();
  renderManualMatrixCanvas();
}

function addManualPair(a, b) {
  if (!Number.isInteger(a) || !Number.isInteger(b)) throw new Error("Укажите оба контакта.");
  if (a < 0 || a >= PIN_COUNT || b < 0 || b >= PIN_COUNT) throw new Error("Контакты должны быть в диапазоне 1..1024.");
  if (a === b) throw new Error("Нельзя соединить контакт сам с собой.");
  if (manualPairIndex(a, b) >= 0) throw new Error("Такая связь уже добавлена.");

  const normalized = { a: Math.min(a, b), b: Math.max(a, b) };
  manualConnections.push({ ...normalized, key: pairKey(normalized.a, normalized.b) });
  manualConnections.sort((left, right) => (left.a - right.a) || (left.b - right.b));
  activeManualPairKey = pairKey(normalized.a, normalized.b);
  selectedManualPins = [];
  if ($("manualContactA")) $("manualContactA").value = "";
  if ($("manualContactB")) $("manualContactB").value = "";
  renderManualEditor();
}

function handleManualCanvasClick(event) {
  const canvas = $("manualMatrixCanvas");
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  const scaleX = canvas.width / rect.width;
  const scaleY = canvas.height / rect.height;
  const x = (event.clientX - rect.left) * scaleX;
  const y = (event.clientY - rect.top) * scaleY;
  const col = Math.max(0, Math.min(MATRIX_SIDE - 1, Math.floor(x / (canvas.width / MATRIX_SIDE))));
  const row = Math.max(0, Math.min(MATRIX_SIDE - 1, Math.floor(y / (canvas.height / MATRIX_SIDE))));
  const pin = row * MATRIX_SIDE + col;

  if (selectedManualPins.length === 1 && selectedManualPins[0] === pin) {
    selectedManualPins = [];
    renderManualEditor();
    return;
  }

  if (!selectedManualPins.length) {
    selectedManualPins = [pin];
    if ($("manualContactA")) $("manualContactA").value = String(pin + 1);
    renderManualEditor();
    return;
  }

  try {
    if ($("manualContactB")) $("manualContactB").value = String(pin + 1);
    addManualPair(selectedManualPins[0], pin);
  } catch (error) {
    selectedManualPins = [];
    toast(error.message);
    renderManualEditor();
  }
}

function manualPinFromEvent(event) {
  const canvas = $("manualMatrixCanvas");
  if (!canvas) return null;
  const rect = canvas.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) return null;
  const scaleX = canvas.width / rect.width;
  const scaleY = canvas.height / rect.height;
  const x = (event.clientX - rect.left) * scaleX;
  const y = (event.clientY - rect.top) * scaleY;
  if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) return null;
  const col = Math.max(0, Math.min(MATRIX_SIDE - 1, Math.floor(x / (canvas.width / MATRIX_SIDE))));
  const row = Math.max(0, Math.min(MATRIX_SIDE - 1, Math.floor(y / (canvas.height / MATRIX_SIDE))));
  return row * MATRIX_SIDE + col;
}

function handleManualCanvasMove(event) {
  const pin = manualPinFromEvent(event);
  if (hoveredManualPin === pin) return;
  hoveredManualPin = pin;
  updateManualHoverInfo(pin);
  renderManualMatrixCanvas();
}

function handleManualCanvasLeave() {
  if (hoveredManualPin === null) return;
  hoveredManualPin = null;
  updateManualHoverInfo(null);
  renderManualMatrixCanvas();
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

function browserTimeObject() {
  const now = new Date();
  return {
    epochMs: now.getTime(),
    offsetMinutes: -now.getTimezoneOffset(),
    timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone || "Browser/Local"
  };
}

function encodeFixedString(value, maxBytes) {
  const encoder = new TextEncoder();
  const result = [];
  for (const char of String(value ?? "")) {
    const chunk = encoder.encode(char);
    if (result.length + chunk.length >= maxBytes) break;
    result.push(...chunk);
  }
  return new Uint8Array(result);
}

function writeFixedString(view, offset, size, value) {
  const bytes = encodeFixedString(value, size);
  for (let i = 0; i < size; ++i) view.setUint8(offset + i, 0);
  bytes.forEach((byte, index) => view.setUint8(offset + index, byte));
}

function parseMappingCrcValue() {
  const text = $("mappingCrc32").value.trim();
  if (!text) return 0;
  const normalized = text.replace(/^0x/i, "");
  if (!/^[0-9a-f]{1,8}$/i.test(normalized)) throw new Error("CRC32 должен содержать до 8 hex-символов.");
  return Number.parseInt(normalized, 16) >>> 0;
}

function sanitizeReferenceFileBase(value) {
  let result = "";
  for (const char of String(value ?? "")) {
    if (result.length >= 60) break;
    if (/[A-Za-z0-9_-]/.test(char)) result += char;
    else if (char === " " || char === ".") result += "_";
  }
  if (!result) result = `reference_${Date.now()}`;
  return result;
}

function makeCrc32Table() {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) {
      c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[n] = c >>> 0;
  }
  return table;
}

const CRC32_TABLE = makeCrc32Table();

function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (const byte of bytes) {
    crc = CRC32_TABLE[(crc ^ byte) & 0xFF] ^ (crc >>> 8);
  }
  return (~crc) >>> 0;
}

function packRle(bytes) {
  const output = [];
  let src = 0;
  while (src < bytes.length) {
    let runLength = 1;
    while (src + runLength < bytes.length && bytes[src + runLength] === bytes[src] && runLength < 128) {
      runLength += 1;
    }

    if (runLength >= 4) {
      output.push(0x80 | (runLength - 1), bytes[src]);
      src += runLength;
      continue;
    }

    const literalStart = src;
    let literalLength = 0;
    while (src < bytes.length && literalLength < 128) {
      runLength = 1;
      while (src + runLength < bytes.length && bytes[src + runLength] === bytes[src] && runLength < 128) {
        runLength += 1;
      }
      if (runLength >= 4 && literalLength > 0) break;
      src += runLength >= 4 ? 0 : 1;
      literalLength += 1;
      if (runLength >= 4) break;
    }

    output.push(literalLength - 1);
    for (let i = 0; i < literalLength; ++i) output.push(bytes[literalStart + i]);
    src = literalStart + literalLength;
  }
  return new Uint8Array(output);
}

function buildManualMatrixBytes() {
  const matrix = new Uint8Array(MATRIX_BYTES);
  const setBit = (row, column) => {
    const offset = row * ROW_BYTES + (column >> 3);
    matrix[offset] |= (1 << (column & 7));
  };
  for (const pair of manualConnections) {
    setBit(pair.a, pair.b);
    setBit(pair.b, pair.a);
  }
  return matrix;
}

function buildPackedPayload(matrixBytes, matrixCrc) {
  const packed = packRle(matrixBytes);
  const payload = new Uint8Array(20 + packed.length);
  const view = new DataView(payload.buffer);
  view.setUint32(0, PACKED_BLOB_MAGIC, true);
  view.setUint16(4, 1, true);
  view.setUint16(6, 0, true);
  view.setUint32(8, matrixBytes.length, true);
  view.setUint32(12, packed.length, true);
  view.setUint32(16, matrixCrc, true);
  payload.set(packed, 20);
  return payload;
}

function buildReferenceHeader(metadata, time, packedPayloadSize, matrixCrc) {
  const buffer = new ArrayBuffer(REFERENCE_HEADER_BYTES);
  const view = new DataView(buffer);

  view.setUint32(0, REFERENCE_MAGIC, true);
  view.setUint16(4, FILE_FORMAT_VERSION, true);
  view.setUint16(6, REFERENCE_HEADER_BYTES, true);
  view.setUint16(8, PIN_COUNT, true);
  view.setUint16(10, ROW_BYTES, true);
  view.setUint32(12, packedPayloadSize, true);
  view.setUint32(16, matrixCrc, true);
  view.setUint32(20, metadata.mappingCrc32 >>> 0, true);
  view.setBigUint64(24, BigInt(time.epochMs), true);
  view.setInt16(32, time.offsetMinutes, true);
  view.setUint16(34, 0, true);

  let offset = 36;
  const writeField = (size, value) => {
    writeFixedString(view, offset, size, value);
    offset += size;
  };

  writeField(64, metadata.name);
  writeField(64, metadata.cableType);
  writeField(32, metadata.revision);
  writeField(64, metadata.deviceId);
  writeField(32, latestDevice?.deviceModel || "KSK-1024");
  writeField(24, latestDevice?.firmwareVersion || "2.1.0");
  writeField(96, metadata.operatorName);
  writeField(64, time.timeZone);
  writeField(256, metadata.comment);
  writeField(24, metadata.approvalStatus);

  return new Uint8Array(buffer);
}

async function uploadGeneratedReference(fileName, fileBytes) {
  const form = new FormData();
  const blob = new Blob([fileBytes], { type: "application/octet-stream" });
  form.append("file", blob, fileName);

  const response = await fetch("/api/upload/reference", {
    method: "POST",
    body: form,
    cache: "no-store"
  });

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
    throw new Error("CRC32 должен содержать до 8 hex-символов.");
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

function collectReferenceMetadata() {
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

  return {
    name: $("referenceName").value.trim(),
    cableType: $("cableType").value.trim(),
    revision: $("revision").value.trim(),
    deviceId: $("deviceId").value.trim(),
    operatorName: $("operatorName").value.trim(),
    comment: $("referenceComment").value.trim(),
    approvalStatus: $("approvalStatus").value,
    mappingCrc32: parseMappingCrcValue()
  };
}

function fillMetadataForm(item) {
  if (!item) return;
  if ($("referenceName")) $("referenceName").value = item.name || "";
  if ($("cableType")) $("cableType").value = item.cableType || "";
  if ($("revision")) $("revision").value = item.revision || "";
  if ($("deviceId")) $("deviceId").value = item.deviceId || "";
  if ($("operatorName")) $("operatorName").value = item.operator || "";
  if ($("mappingCrc32")) $("mappingCrc32").value = item.mappingCrc32 ? Number(item.mappingCrc32).toString(16).toUpperCase() : "";
  if ($("approvalStatus")) $("approvalStatus").value = item.approvalStatus || "draft";
  rememberCurrentMetadata();
}

async function openReferenceForEditing(fileName) {
  try {
    const [item, view] = await Promise.all([
      Promise.resolve(currentReferences.find((entry) => entry.file === fileName) || null),
      api(`/api/reference/view?file=${encodeURIComponent(fileName)}`)
    ]);

    if (item) {
      fillMetadataForm(item);
      renderReferencePreview(item);
      editingReferenceName = item.name || fileName;
    } else {
      editingReferenceName = fileName;
    }

    editingReferenceFile = fileName;
    manualConnections = (Array.isArray(view?.connections) ? view.connections : [])
      .map((pair) => ({
        a: Number(pair.a),
        b: Number(pair.b),
        key: pairKey(Number(pair.a), Number(pair.b))
      }))
      .filter((pair) => Number.isInteger(pair.a) && Number.isInteger(pair.b) && pair.a >= 0 && pair.b >= 0 && pair.a < PIN_COUNT && pair.b < PIN_COUNT);
    manualConnections.sort((left, right) => (left.a - right.a) || (left.b - right.b));
    selectedManualPins = [];
    activeManualPairKey = "";
    if ($("manualContactA")) $("manualContactA").value = "";
    if ($("manualContactB")) $("manualContactB").value = "";
    renderManualEditor();
    $("saveManualReferenceButton")?.scrollIntoView({ behavior: "smooth", block: "center" });
    toast(`Эталон открыт для редактирования: ${editingReferenceName}`);
  } catch (error) {
    toast(error.message);
  }
}

async function saveManualReference() {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  if (!manualConnections.length) return toast("Добавьте хотя бы одну связь в ручной эталон.");

  try {
    const metadata = collectReferenceMetadata();
    const time = browserTimeObject();
    rememberCurrentMetadata();

    const matrixBytes = buildManualMatrixBytes();
    const matrixCrc = crc32(matrixBytes);
    const packedPayload = buildPackedPayload(matrixBytes, matrixCrc);
    const header = buildReferenceHeader(metadata, time, packedPayload.length, matrixCrc);
    const fileBytes = new Uint8Array(header.length + packedPayload.length);
    fileBytes.set(header, 0);
    fileBytes.set(packedPayload, header.length);

    let fileName = `${sanitizeReferenceFileBase(metadata.name)}.ref`;
    if (editingReferenceFile) {
      const overwrite = confirm(
        `Перезаписать открытый эталон ${editingReferenceName || editingReferenceFile}?\n\nНажмите OK для перезаписи.\nНажмите Отмена, чтобы сохранить как новый файл.`
      );
      if (overwrite) fileName = editingReferenceFile;
    }
    const response = await uploadGeneratedReference(fileName, fileBytes);
    editingReferenceFile = response.file || fileName;
    editingReferenceName = metadata.name;
    renderManualEditor();
    toast(`Ручной эталон сохранен: ${response.file || fileName}`);
    await loadReferences();
  } catch (error) {
    toast(error.message);
  }
}

function sortReferenceReports(files) {
  const sortMode = $("referenceSort")?.value || "date_desc";
  const reports = [...files];
  reports.sort((left, right) => {
    const leftDate = Date.parse(left?.createdAtLocal || "") || 0;
    const rightDate = Date.parse(right?.createdAtLocal || "") || 0;
    if (sortMode === "date_asc") return leftDate - rightDate;
    if (sortMode === "name_asc") return String(left.name || left.file).localeCompare(String(right.name || right.file), "ru");
    if (sortMode === "name_desc") return String(right.name || right.file).localeCompare(String(left.name || left.file), "ru");
    return rightDate - leftDate;
  });
  return reports;
}

function renderReferencePreview(item) {
  const target = $("referencePreview");
  if (!target) return;
  if (!item) {
    target.innerHTML = '<div class="message">Выберите эталон в журнале, чтобы посмотреть его данные.</div>';
    return;
  }

  target.innerHTML = `
    <div class="admin-preview-grid">
      <div class="admin-preview-item"><strong>Название</strong><span>${escapeHtml(item.name || item.file)}</span></div>
      <div class="admin-preview-item"><strong>Дата</strong><span>${escapeHtml(formatStoredDate(item.createdAtLocal || ""))}</span></div>
      <div class="admin-preview-item"><strong>Тип сборки</strong><span>${escapeHtml(item.cableType || "не указан")}</span></div>
      <div class="admin-preview-item"><strong>Ревизия</strong><span>${escapeHtml(item.revision || "не указана")}</span></div>
      <div class="admin-preview-item"><strong>Прибор</strong><span>${escapeHtml(item.deviceId || "не указан")}</span></div>
      <div class="admin-preview-item"><strong>Оператор</strong><span>${escapeHtml(item.operator || "не указан")}</span></div>
      <div class="admin-preview-item"><strong>Статус</strong><span>${escapeHtml(approvalStatusLabel(item.approvalStatus))}</span></div>
      <div class="admin-preview-item"><strong>Размер</strong><span>${escapeHtml(fileSize(item.size || 0))}</span></div>
      <div class="admin-preview-item"><strong>CRC таблицы</strong><span>${escapeHtml(item.mappingCrc32 ? String(item.mappingCrc32) : "не указан")}</span></div>
      <div class="admin-preview-item"><strong>CRC матрицы</strong><span>${escapeHtml(item.matrixCrc32 ? String(item.matrixCrc32) : "не указан")}</span></div>
    </div>
  `;
}

function setAdminAccess(enabled) {
  serviceUnlockEnabled = Boolean(enabled);
  const note = $("serviceLockNote");
  const badge = $("serviceBadge");
  const text = $("serviceText");

  if (badge) {
    badge.textContent = serviceUnlockEnabled ? "Сервисный режим включен" : "Сервисный режим выключен";
    badge.className = `badge ${serviceUnlockEnabled ? "ok" : "error"}`;
  }
  if (text) {
    text.textContent = serviceUnlockEnabled
      ? "Создание эталонов, загрузка файлов и удаление записей доступны."
      : "Создание, загрузка и удаление заблокированы до включения сервисного тумблера.";
  }
  if (note) note.hidden = serviceUnlockEnabled;

  ["captureButton", "saveManualReferenceButton", "referenceUploadButton", "calculationUploadButton"].forEach((id) => {
    if ($(id)) $(id).hidden = !serviceUnlockEnabled;
  });
  ["referenceUploadInput", "calculationUploadInput"].forEach((id) => {
    if ($(id)) $(id).hidden = !serviceUnlockEnabled;
  });

  renderReferences(currentReferences);
}

async function loadDevice() {
  try {
    const device = await api("/api/device");
    latestDevice = device;
    const badge = $("deviceBadge");
    badge.textContent = `${device.deviceModel || device.hardware} · ${device.ip} · ${device.link ? "LINK" : "NO LINK"} · FW ${device.firmwareVersion || "?"}`;
    badge.className = `badge ${device.link ? "ok" : "error"}`;

    const engineBadge = $("engineBadge");
    const engineText = $("engineText");
    if (engineBadge) {
      const kind = device.engineBusy ? "busy" : device.engineState === "failed" ? "error" : device.engineState === "idle" ? "ok" : "neutral";
      engineBadge.className = `badge ${kind}`;
      engineBadge.textContent = device.engineBusy ? "Идет операция" : (device.engineState || "idle");
    }
    if (engineText) {
      engineText.textContent = device.engineMessage || "Система готова.";
    }

    setAdminAccess(device.serviceUnlockEnabled);
  } catch (error) {
    latestDevice = null;
    $("deviceBadge").textContent = "Нет связи";
    $("deviceBadge").className = "badge error";
    $("engineBadge").textContent = "Нет связи";
    $("engineBadge").className = "badge error";
    $("engineText").textContent = "Не удалось получить состояние устройства.";
    setAdminAccess(false);
  }
}

async function deleteReferenceFile(fileName) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  if (!confirm(`Удалить эталон ${fileName}?`)) return;
  try {
    await api(`/api/reference?file=${encodeURIComponent(fileName)}`, { method: "DELETE" });
    toast("Эталон удален.");
    await loadReferences();
  } catch (error) {
    toast(error.message);
  }
}

function renderReferences(files) {
  const target = $("referenceTable");
  const summary = $("referenceSummary");
  const pagination = $("referencePagination");
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

  const totalPages = Math.max(1, Math.ceil(reports.length / REFERENCE_PAGE_SIZE));
  referencePage = Math.min(Math.max(referencePage, 1), totalPages);
  const start = (referencePage - 1) * REFERENCE_PAGE_SIZE;
  const pageReports = reports.slice(start, start + REFERENCE_PAGE_SIZE);

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
    <button type="button" data-reference-page="prev" ${referencePage <= 1 ? "disabled" : ""}>Назад</button>
    <span class="journal-pagination-info">Страница ${referencePage} из ${totalPages}</span>
    <button type="button" data-reference-page="next" ${referencePage >= totalPages ? "disabled" : ""}>Вперед</button>
  `;

  target.querySelectorAll("[data-open-reference]").forEach((button) => {
    button.addEventListener("click", async () => {
      await openReferenceForEditing(button.dataset.openReference);
    });
  });
  target.querySelectorAll("[data-delete-reference]").forEach((button) => {
    button.addEventListener("click", async () => {
      await deleteReferenceFile(button.dataset.deleteReference);
    });
  });
  pagination.querySelectorAll("[data-reference-page]").forEach((button) => {
    button.addEventListener("click", () => {
      referencePage += button.dataset.referencePage === "next" ? 1 : -1;
      renderReferences(files);
    });
  });
}

async function loadReferences() {
  try {
    currentReferences = await api("/api/references");
    primeSuggestionsFromReferences(currentReferences);
    refreshInputSuggestions();
    renderReferences(currentReferences);
  } catch (error) {
    toast(error.message);
  }
}

function uploadFile(kind, input, bar, text) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  const file = input.files?.[0];
  if (!file) {
    toast("Сначала выберите файл.");
    return;
  }

  const form = new FormData();
  form.append("file", file, file.name);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", `/api/upload/${kind}`, true);

  input.disabled = true;
  bar.style.width = "0%";
  text.textContent = `Загрузка: ${file.name}`;

  xhr.upload.onprogress = (event) => {
    if (!event.lengthComputable) return;
    const percent = Math.max(0, Math.min(100, Math.round((event.loaded / event.total) * 100)));
    bar.style.width = `${percent}%`;
    text.textContent = `Загрузка: ${file.name} (${percent}%)`;
  };

  xhr.onload = async () => {
    input.disabled = false;
    const response = xhr.responseText ? JSON.parse(xhr.responseText) : {};
    if (xhr.status >= 200 && xhr.status < 300) {
      bar.style.width = "100%";
      text.textContent = `Загружено: ${response.file || file.name}`;
      input.value = "";
      toast("Файл проверен и сохранен.");
      await loadReferences();
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
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  try {
    const params = referenceMetadataParams();
    localStorage.setItem("cableTesterDeviceId", $("deviceId").value.trim());
    localStorage.setItem("cableTesterOperator", $("operatorName").value.trim());
    rememberCurrentMetadata();
    await api(`/api/reference/capture?${params}`, { method: "POST" });
    toast("Эталонный замер запущен. После завершения он появится в журнале.");
    setTimeout(loadReferences, 2500);
    setTimeout(loadDevice, 1200);
  } catch (error) {
    toast(error.message);
  }
});

$("saveManualReferenceButton").addEventListener("click", saveManualReference);

$("addManualPairButton").addEventListener("click", () => {
  try {
    const a = parseContactValue($("manualContactA").value);
    const b = parseContactValue($("manualContactB").value);
    addManualPair(a, b);
  } catch (error) {
    toast(error.message);
  }
});

$("clearManualPairsButton").addEventListener("click", () => {
  manualConnections = [];
  selectedManualPins = [];
  activeManualPairKey = "";
  if ($("manualContactA")) $("manualContactA").value = "";
  if ($("manualContactB")) $("manualContactB").value = "";
  renderManualEditor();
});

if ($("manualMatrixCanvas")) {
  $("manualMatrixCanvas").addEventListener("click", handleManualCanvasClick);
  $("manualMatrixCanvas").addEventListener("mousemove", handleManualCanvasMove);
  $("manualMatrixCanvas").addEventListener("mouseleave", handleManualCanvasLeave);
}

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

$("refreshAdminButton").addEventListener("click", async () => {
  await loadDevice();
  await loadReferences();
});

$("refreshReferencesButton").addEventListener("click", loadReferences);
$("referenceSort").addEventListener("change", () => {
  referencePage = 1;
  renderReferences(currentReferences);
});

for (const id of ["deviceId", "operatorName"]) {
  if ($(id)) $(id).addEventListener("change", rememberCurrentMetadata);
}

$("deviceId").value = localStorage.getItem("cableTesterDeviceId") || "";
$("operatorName").value = localStorage.getItem("cableTesterOperator") || "";
refreshInputSuggestions();
updateManualHoverInfo(null);
renderManualEditor();

loadDevice();
loadReferences();
setInterval(loadDevice, 10000);

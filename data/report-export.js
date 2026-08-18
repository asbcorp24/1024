// Экспорт одного отчёта в тот же CSV-формат, который принимает
// ручной конструктор эталона в admin.js.
// Загружается после app.js и переопределяет exportCalculationReportCsv().

(function () {
  function pairKey(a, b) {
    const left = Math.min(Number(a), Number(b));
    const right = Math.max(Number(a), Number(b));
    return `${left}:${right}`;
  }

  function endpointMapFromAnnotations(annotations) {
    const map = new Map();

    const remember = (pin, mark, localPin) => {
      const numericPin = Number(pin);
      if (!Number.isInteger(numericPin) || numericPin < 0 || numericPin > 1023) return;
      const markText = String(mark ?? "").trim();
      const localText = String(localPin ?? "").trim();
      if (!markText || !localText) return;
      if (!map.has(numericPin)) map.set(numericPin, { mark: markText, localPin: localText });
    };

    for (const item of Array.isArray(annotations) ? annotations : []) {
      remember(item?.a, item?.markA, item?.localPinA);
      remember(item?.b, item?.markB, item?.localPinB);
    }

    return map;
  }

  function endpointFor(pair, side, pin, endpointMap) {
    const mark = String(pair?.[`mark${side}`] ?? "").trim();
    const localPin = String(pair?.[`localPin${side}`] ?? "").trim();
    const known = endpointMap.get(Number(pin));

    return {
      mark: mark || known?.mark || "КСК",
      localPin: localPin || known?.localPin || String(Number(pin) + 1)
    };
  }

  function splitPositionFromNote(value) {
    const note = String(value ?? "").trim();
    const match = note.match(/^позиция\s*:\s*([^;]+)(?:;\s*(.*))?$/i);
    if (!match) return { position: "", note };
    return {
      position: String(match[1] || "").trim(),
      note: String(match[2] || "").trim()
    };
  }

  // Функция объявлена в app.js как глобальная function declaration.
  // Здесь намеренно заменяем только её реализацию.
  exportCalculationReportCsv = async function exportCalculationReportCsvCompatible(fileName) {
    try {
      const [result, view] = await Promise.all([
        api(`/api/result?file=${encodeURIComponent(fileName)}`),
        api(`/api/result/view?file=${encodeURIComponent(fileName)}`)
      ]);

      const annotations = Array.isArray(view?.annotations) ? view.annotations : [];
      const endpointMap = endpointMapFromAnnotations(annotations);
      const extraKeys = new Set((Array.isArray(view?.extraConnections) ? view.extraConnections : [])
        .map((item) => pairKey(item?.a, item?.b)));
      const asymmetricKeys = new Set((Array.isArray(view?.asymmetricConnections) ? view.asymmetricConnections : [])
        .map((item) => pairKey(item?.a, item?.b)));

      // connections — фактически измеренные двусторонние соединения.
      // Асимметричные пары добавляем отдельно, чтобы они не пропали из CSV.
      const measuredPairs = [];
      const seen = new Set();

      for (const item of Array.isArray(view?.connections) ? view.connections : []) {
        const key = pairKey(item?.a, item?.b);
        if (seen.has(key)) continue;
        seen.add(key);
        measuredPairs.push(item);
      }

      for (const item of Array.isArray(view?.asymmetricConnections) ? view.asymmetricConnections : []) {
        const key = pairKey(item?.a, item?.b);
        if (seen.has(key)) continue;
        seen.add(key);
        measuredPairs.push(item);
      }

      const rows = [[
        "Провод",
        "Позиция",
        "Маркировка",
        "Контакт",
        "Маркировка",
        "Контакт",
        "Примечание"
      ]];

      for (const pair of measuredPairs) {
        const a = Number(pair?.a ?? pair?.source);
        const b = Number(pair?.b ?? pair?.target);
        if (!Number.isInteger(a) || !Number.isInteger(b) || a < 0 || b < 0 || a > 1023 || b > 1023 || a === b) continue;

        const endpointA = endpointFor(pair, "A", a, endpointMap);
        const endpointB = endpointFor(pair, "B", b, endpointMap);
        const parsedNote = splitPositionFromNote(pair?.note);
        const statusNotes = [];
        const key = pairKey(a, b);

        if (parsedNote.note) statusNotes.push(parsedNote.note);
        if (extraKeys.has(key)) statusNotes.push("Паразитная связь по результату проверки");
        if (asymmetricKeys.has(key)) statusNotes.push("Асимметрия по результату проверки");

        rows.push([
          String(pair?.wireName ?? "").trim(),
          parsedNote.position,
          endpointA.mark,
          endpointA.localPin,
          endpointB.mark,
          endpointB.localPin,
          statusNotes.join("; ")
        ]);
      }

      const exportName = String(fileName || "report.json").replace(/\.json$/i, ".csv");
      downloadTextFile(
        exportName,
        rows.map((row) => row.map(csvEscape).join(";")).join("\n"),
        "text/csv;charset=utf-8"
      );

      toast(`CSV выгружен в формате импорта эталона: ${Math.max(0, rows.length - 1)} связей.`);
    } catch (error) {
      toast(error.message);
    }
  };
})();

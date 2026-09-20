//------------------------------------------------------------------------------
// Web-Interface (Plotly Diagramm) im Flash-Speicher hinterlegt
// -----------------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Tibber Preisverlauf</title>
    <script src="https://cdn.plot.ly/plotly-latest.min.js" charset="utf-8"></script>
    <style>
        body { font-family: Arial, sans-serif; background-color: #111; color: #fff; margin: 0; padding: 20px; }
        h2 { text-align: center; color: #00d1b2; margin-bottom: 5px; }
        .sub { text-align: center; color: #aaa; margin-bottom: 15px; font-size: 0.9em; }
        .nav-container { display: flex; justify-content: center; align-items: center;
			gap: 40px; max-width: 1000px; margin: 0 auto 20px auto; }
		.nav-group { display: flex; align-items: center; gap: 10px; }
		.action-group { display: flex; align-items: center; gap: 10px; }
        button { background-color: #00d1b2; color: #111; border: none; padding: 10px 20px; font-size: 1em; font-weight: bold; border-radius: 4px; cursor: pointer; transition: 0.2s; }
        button:hover { background-color: #00b89c; }
        button:disabled { background-color: #444; color: #888; cursor: not-allowed; }
        #date-label { font-size: 1.2em; font-weight: bold; align-self: center; min-width: 150px; text-align: center; }
        #chart { width: 100%; max-width: 1000px; height: 500px; margin: 0 auto; }
        .loading { text-align: center; color: #ffdd57; font-size: 1.2em; margin-top: 50px; }
    </style>
</head>
<body>

    <h2>Marstek-Tibber: Energie-Dashboard</h2>
    <div class="sub">Historischer Verlauf & Peak-Steuerung</div>

    <!-- NEU: Buttons zum Blättern, Exportieren und Importieren der Historie -->
    <div class="nav-container">
	  <div class="nav-group">
        <!-- 14 Tage zurück -->
        <button id="btn-prev-14" onclick="changeDay(-14)">⏮ 2 Wo</button>
        
        <!-- Standard Tages-Navigation -->
        <button id="btn-prev" onclick="changeDay(-1)">◀ Vorheriger Tag</button>
        <div id="date-label">Lade...</div>
        <button id="btn-next" onclick="changeDay(1)">Nächster Tag ▶</button>
        
        <!-- 14 Tage vor -->
        <button id="btn-next-14" onclick="changeDay(14)">2 Wo ⏭</button>
      </div>
	  <div class="action-group">
        <!-- JSON Export Button -->
        <button id="btn-download-json" onclick="downloadJSON()">💾 Save</button>
        <!-- JSON Import (Verstecktes Input + Styled Button) -->
        <input type="file" id="import-file" accept=".json" onchange="importJSON(event)" style="display: none;">
        <button id="btn-import-json" onclick="document.getElementById('import-file').click()">📂 Open</button>
	  </div>
    </div>


    <div id="loading" class="loading">Lade Energiedaten vom ESP32...</div>
    <div id="chart"></div>

    <script>
        let allDaysData = {}; // Gruppierte Daten nach Datum {"2026-09-12": [...]}
        let uniqueDays = [];  // Sortierte Liste der verfügbaren Tage ["2026-09-11", "2026-09-12", ...]
        let currentDayIndex = 0;

        async function loadData() {
            try {
                const response = await fetch('/prices');
                const data = await response.json();
                
                if (!data || !Array.isArray(data) || data.length === 0) {
                    document.getElementById('loading').innerText = 'Keine Daten verfügbar.';
                    return;
                }

                document.getElementById('loading').style.display = 'none';

                // Frische ESP32-Daten einpflegen. overrideExisting = true sorgt dafür,
                // dass Live-Daten des ESP32 die Struktur initial befüllen oder aktualisieren.
                mergeDataRecords(data, true);

                // Standardmäßig den LETZTEN Tag (also den aktuellsten/heutigen) anzeigen
                currentDayIndex = uniqueDays.length - 1;
                
                updateChart();

            } catch (error) {
                document.getElementById('loading').innerText = 'Fehler beim Laden: ' + error;
            }
        }

        function changeDay(direction) {
            let newIndex = currentDayIndex + direction;
            if (newIndex >= 0 && newIndex < uniqueDays.length) {
                currentDayIndex = newIndex;
                updateChart();
            }
        }

        function updateChart() {
            const currentDay = uniqueDays[currentDayIndex];
            
            // Buttons aktivieren/deaktivieren je nach Grenzen
            document.getElementById('btn-prev').disabled    = (currentDayIndex === 0);
            document.getElementById('btn-next').disabled    = (currentDayIndex === uniqueDays.length - 1);
            document.getElementById('btn-prev-14').disabled = (currentDayIndex - 14 < 0);
            document.getElementById('btn-next-14').disabled = (currentDayIndex + 14 >= uniqueDays.length);
            
            // Datumsanzeige formatieren (Jahr für Anzeige wegschneiden: "09-12")
            document.getElementById('date-label').innerText = currentDay.substring(5);

            const dayData = allDaysData[currentDay];
            const times = [];
            const prices = [];
            const powers = [];
            const colors = [];
            let thresholdPriceCent = null;

            dayData.forEach(item => {
                const timePart = item.time.substring(11); // Nur die Uhrzeit "14:15"
                times.push(timePart);
                
                const priceCent = item.price * 100;
                prices.push(priceCent);
                powers.push(item.power !== undefined ? item.power : 0);
                colors.push(item.isExpensive ? '#ff3860' : '#23d160');

                if (item.isExpensive) {
                    if (thresholdPriceCent === null || priceCent < thresholdPriceCent) {
                        thresholdPriceCent = priceCent;
                    }
                }
            });

            const priceTrace = {
                x: times, y: prices, type: 'bar', name: 'Preis (ct/kWh)',
                marker: { color: colors },
                hovertemplate: '<b>Uhrzeit:</b> %{x}<br><b>Preis:</b> %{y:.2f} ct/kWh<extra></extra>'
            };

            const powerTrace = {
                x: times, y: powers, type: 'scatter', mode: 'lines+markers', name: 'Leistung (Watt)',
                line: { color: '#3273dc', width: 3 }, marker: { size: 4 },
                yaxis: 'y2',
                hovertemplate: '<b>Verbrauch:</b> %{y} W<extra></extra>'
            };

            const layout = {
                paper_bgcolor: '#111', plot_bgcolor: '#111', font: { color: '#fff' },
                margin: { t: 40, r: 60, l: 60, b: 60 },
                showlegend: true, legend: { x: 0, y: 1.15, orientation: 'h' },
                xaxis: {
                    title: 'Uhrzeit', gridcolor: '#333', tickangle: -45,
                    type: 'category', tickmode: 'linear', dtick: 8 
                },
                yaxis: { title: 'Preis (Cent / kWh)', gridcolor: '#333', side: 'left' },
                yaxis2: {
                    title: 'Effektive Leistung (Watt)', gridcolor: '#222', side: 'right',
                    overlaying: 'y', zerolinecolor: '#555', zerolinewidth: 2
                },
                shapes: (thresholdPriceCent !== null) ? [{
                    type: 'line', xref: 'paper', x0: 0, x1: 1, y0: thresholdPriceCent, y1: thresholdPriceCent,
                    line: { color: '#ffdd57', width: 2, dash: 'dashdot' }
                }] : []
            };

            Plotly.newPlot('chart', [priceTrace, powerTrace], layout, { responsive: true, displayModeBar: false });
        }

        // Funktion 1: Exportiert alle aktuell im Browser befindlichen Daten als JSON
        function downloadJSON() {
            const allDataFlat = [];
            Object.keys(allDaysData).sort().forEach(day => {
                allDataFlat.push(...allDaysData[day]);
            });

            // NEU & ZUKUNFTSSICHER: Nutzung eines Blobs statt einer langen URL-Zeichenkette
            const jsonString = JSON.stringify(allDataFlat, null, 2);
            const blob = new Blob([jsonString], { type: "application/json;charset=utf-8;" });
            const blobUrl = URL.createObjectURL(blob);

            const downloadAnchor = document.createElement('a');
            downloadAnchor.setAttribute("href", blobUrl);
            downloadAnchor.setAttribute("download", "tibber_database_" + new Date().toISOString().substring(0,10) + ".json");
            document.body.appendChild(downloadAnchor);
            downloadAnchor.click();
            
            // Speicher im Browser direkt wieder freigeben
            document.body.removeChild(downloadAnchor);
            URL.revokeObjectURL(blobUrl);
        }

        // Funktion 2: Hilfsfunktion, um Daten konfliktfrei in die bestehende Struktur zu mergen
        // WICHTIG: ESP32-Daten behalten Vorrang (overrideExisting = false beim Import)!
        // Unterstützt automatisches Befüllen von Null-Werten und Nutzerabfragen bei Konflikten
        // Verhindert das Überschreiben von Messwerten durch Nullen bei nicht-chronologischen Imports
        async function mergeDataRecords(newDataArray, overrideExisting) {
            let addedCount = 0;
            let skippedCount = 0;
            let userDecision = null; // Speichert die Entscheidung des Nutzers für diesen Import-Durchlauf

            for (const item of newDataArray) {
                if (!item.time) continue;
                
                const dayKey = item.time.substring(0, 10); // Extrahierte Gruppe: "2026-09-14"
                
                if (!allDaysData[dayKey]) {
                    allDaysData[dayKey] = [];
                }

                // Prüfen, ob für diese exakte Uhrzeit bereits ein Eintrag existiert
                const existingIndex = allDaysData[dayKey].findIndex(existingItem => existingItem.time === item.time);

                if (existingIndex === -1) {
                    // --- FALL 1: Kein Eintrag vorhanden ---
                    allDaysData[dayKey].push(item);
                    addedCount++;
                } else {
                    // Eintrag existiert bereits!
                    const existingItem = allDaysData[dayKey][existingIndex];
                    
                    if (overrideExisting) {
                        // ESP32-Live-Ladevorgang hat immer absolute Priorität
                        allDaysData[dayKey][existingIndex] = item;
                        addedCount++;
                        continue;
                    }

                    // --- FALL 2: Umgang mit Nullwerten (Automatische Auflösung) ---
                    
                    // Unterfall A: Bestehend ist Null (oder fehlt), Import hat echte Daten -> Import annehmen
                    if ((existingItem.power === 0 || existingItem.power === undefined) && item.power > 0) {
                        allDaysData[dayKey][existingIndex] = item;
                        addedCount++;
                        continue;
                    }

                    // Unterfall B: Bestehend hat echte Daten (>0), Import ist Null -> Bestehende Daten behalten
                    if (existingItem.power > 0 && item.power === 0) {
                        skippedCount++;
                        continue; // Lautlos überspringen, bestehender Wert bleibt geschützt!
                    }

                    // --- FALL 3: Daten sind identisch ---
                    if (existingItem.price === item.price && existingItem.power === item.power) {
                        skippedCount++;
                        continue;
                    }

                    // --- FALL 4: ECHTER WIDERSPRUCH (Beide Werte sind ungleich Null und voneinander verschieden) ---
                    if (userDecision === null) {
                        const text = `Konflikt bei ${item.time} entdeckt!\n` +
                                     `Bestehend: ${existingItem.price} ct | ${existingItem.power} W\n` +
                                     `Import: ${item.price} ct | ${item.power} W\n\n` +
                                     `Möchtest du alle widersprüchlichen Daten dieser Datei überschreiben?\n` +
                                     `[OK] = Import-Daten annehmen\n` +
                                     `[Abbrechen] = Bestehende Daten behalten`;
                        
                        const confirmChoice = confirm(text);
                        userDecision = confirmChoice ? 'overwrite' : 'keep';
                    }

                    if (userDecision === 'overwrite') {
                        allDaysData[dayKey][existingIndex] = item;
                        addedCount++;
                    } else {
                        skippedCount++;
                    }
                }
            }

            // Nach dem Einfügen müssen die Einträge innerhalb der Tage chronologisch sortiert werden
            Object.keys(allDaysData).forEach(dayKey => {
                allDaysData[dayKey].sort((a, b) => a.time.localeCompare(b.time));
            });

            // Liste der verfügbaren Tage für die Navigation aktualisieren
            uniqueDays = Object.keys(allDaysData).sort();
            
            console.log(`Merge abgeschlossen: ${addedCount} hinzugefügt/aktualisiert, ${skippedCount} übersprungen/behalten.`);
        }


        // Funktion 3: Liest die ausgewählte JSON-Datei ein und stößt den Merge an
        function importJSON(event) {
            const file = event.target.files[0];
            if (!file) return;

            const reader = new FileReader();
            reader.onload = function(e) {
                try {
                    const importedData = JSON.parse(e.target.result);
                    
                    if (!Array.isArray(importedData)) {
                        alert("Fehler: Die JSON-Datei muss ein Array von Datensätzen enthalten.");
                        return;
                    }

                    // Führe den Import aus. overrideExisting = false sorgt dafür,
                    // dass die Daten vom ESP32 auf keinen Fall überschrieben werden!
                    mergeDataRecords(importedData, false);
                    
                    // Nach dem Import den Index wieder auf den allerneuesten Tag setzen!
                    currentDayIndex = uniqueDays.length - 1;

                    // Diagramm und Datumsanzeige auf den neuesten Stand bringen
                    updateChart();
                    alert("Historie erfolgreich geladen und mit ESP32-Daten verschmolzen!");

                } catch (error) {
                    alert("Fehler beim Parsen der JSON-Datei: " + error.message);
                }
            };
            reader.readAsText(file);
            
            // Input zurücksetzen, damit dieselbe Datei direkt nochmal gewählt werden könnte
            event.target.value = '';
        }

        window.addEventListener('DOMContentLoaded', loadData);
    </script>
</body>
</html>
)=====";

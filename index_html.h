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
        .nav-container { display: flex; justify-content: center; gap: 20px; margin-bottom: 20px; }
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

    <!-- NEU: Buttons zum Blättern durch die Tage -->
    <div class="nav-container">
        <button id="btn-prev" onclick="changeDay(-1)">◀ Vorheriger Tag</button>
        <div id="date-label">Lade...</div>
        <button id="btn-next" onclick="changeDay(1)">Nächster Tag ▶</button>
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

                // Daten nach Datum gruppieren (Jahr wegschneiden für die Anzeige)
                allDaysData = {};
                data.forEach(item => {
                    if (item.time) {
                        const dayKey = item.time.substring(0, 10); // "2026-09-12"
                        if (!allDaysData[dayKey]) {
                            allDaysData[dayKey] = [];
                        }
                        allDaysData[dayKey].push(item);
                    }
                });

                // Verfügbare Tage ermitteln und sortieren
                uniqueDays = Object.keys(allDaysData).sort();
                
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
            document.getElementById('btn-prev').disabled = (currentDayIndex === 0);
            document.getElementById('btn-next').disabled = (currentDayIndex === uniqueDays.length - 1);
            
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

                if (item.isExpensive && thresholdPriceCent === null) {
                    thresholdPriceCent = priceCent;
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

        window.addEventListener('DOMContentLoaded', loadData);
    </script>
</body>
</html>
)=====";

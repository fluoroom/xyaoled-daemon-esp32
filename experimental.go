// Package experimental contains test utilities for investigating the XYAOLED protocol.
// This file provides a web interface for testing GIF frame handling and protocol analysis.
package main

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"image/gif"
	"io"
	"net/http"
	"strconv"
	"strings"
	"xyaoled-daemon/xyaoled"
)

// GifAnalysis holds information about a GIF file
type GifAnalysis struct {
	FrameCount    int
	TotalSize     int
	Dimensions    string
	Delays        []int
	TotalDuration int // milliseconds
	LoopCount     int
}

// analyzeGif extracts metadata from a GIF buffer
func analyzeGif(data []byte) (*GifAnalysis, error) {
	g, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}

	analysis := &GifAnalysis{
		FrameCount: len(g.Image),
		TotalSize:  len(data),
		Delays:     g.Delay,
		LoopCount:  g.LoopCount,
	}

	if len(g.Image) > 0 {
		bounds := g.Image[0].Bounds()
		analysis.Dimensions = fmt.Sprintf("%dx%d", bounds.Dx(), bounds.Dy())
	}

	for _, d := range g.Delay {
		analysis.TotalDuration += d * 10 // centiseconds to milliseconds
	}

	return analysis, nil
}

// HandleUploadTestGif serves the experimental GIF testing interface
func (h *Handler) HandleUploadTestGif(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		h.serveGifTestForm(w, r)
		return
	}

	if r.Method == http.MethodPost {
		h.processGifTest(w, r)
		return
	}

	http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
}

func (h *Handler) serveGifTestForm(w http.ResponseWriter, r *http.Request) {
	tmpl := `<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XYAOLED GIF Test - Experimental</title>
<style>
body { font-family: system-ui, sans-serif; max-width: 1200px; margin: 20px auto; padding: 0 20px; background: #0a0a0a; color: #e0e0e0; }
h1 { color: #4CAF50; border-bottom: 2px solid #4CAF50; padding-bottom: 10px; }
h2 { color: #FFC107; margin-top: 30px; }
.section { background: #1a1a1a; padding: 20px; margin: 20px 0; border-radius: 8px; border: 1px solid #333; }
.form-group { margin: 15px 0; }
label { display: block; margin-bottom: 5px; color: #4CAF50; font-weight: 500; }
input[type="file"] { display: block; margin: 10px 0; padding: 10px; background: #2a2a2a; border: 1px solid #444; border-radius: 4px; color: #e0e0e0; width: 100%; }
input[type="number"], select { padding: 8px; background: #2a2a2a; border: 1px solid #444; border-radius: 4px; color: #e0e0e0; width: 200px; }
button { padding: 12px 24px; background: #4CAF50; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; margin: 5px; }
button:hover { background: #45a049; }
button.secondary { background: #FFC107; color: #000; }
button.secondary:hover { background: #FFB300; }
.analysis { background: #2a2a2a; padding: 15px; border-radius: 4px; margin: 10px 0; border-left: 4px solid #4CAF50; }
.analysis pre { margin: 0; color: #4CAF50; font-family: 'Courier New', monospace; }
.protocol-dump { background: #1a1a1a; padding: 15px; border-radius: 4px; overflow-x: auto; border: 1px solid #333; }
.protocol-dump pre { color: #FFC107; font-size: 12px; margin: 0; }
.warning { background: #ff5722; color: white; padding: 10px; border-radius: 4px; margin: 10px 0; }
.info { background: #2196F3; color: white; padding: 10px; border-radius: 4px; margin: 10px 0; }
.result { margin: 20px 0; padding: 15px; background: #2a2a2a; border-radius: 4px; }
.result.success { border-left: 4px solid #4CAF50; }
.result.error { border-left: 4px solid #ff5722; }
table { width: 100%; border-collapse: collapse; margin: 15px 0; }
th, td { padding: 10px; text-align: left; border-bottom: 1px solid #333; }
th { background: #2a2a2a; color: #4CAF50; }
tr:hover { background: #2a2a2a; }
</style>
</head>
<body>
<h1>🔬 XYAOLED GIF Test - Experimental Protocol Analysis</h1>

<div class="info">
<strong>Investigation Goal:</strong> Analyze how the proprietary app sends large multi-frame GIFs (20+ frames) 
while our implementation only processes 6 frames from an 18-frame GIF.
</div>

<div class="section">
<h2>📤 Upload & Analyze GIF</h2>
<form method="POST" enctype="multipart/form-data">
<div class="form-group">
<label>Upload GIF File:</label>
<input type="file" name="gif" accept="image/gif" required>
</div>

<div class="form-group">
<label>Send Mode:</label>
<select name="mode" id="mode">
<option value="standard">Standard (Current Implementation)</option>
<option value="chunked-88ff">🚀 Chunked 88ff Protocol (NEW - 18+ frames!)</option>
<option value="raw">Raw (No Processing)</option>
<option value="noresize">No Resize (Keep Original Size)</option>
<option value="chunked">Chunked Upload (Test Pacing - Legacy)</option>
</select>
</div>

<div class="form-group">
<label>Force Reinit:</label>
<input type="checkbox" name="reinit" id="reinit" checked> 
<span style="color: #999;">Clear device state before send</span>
</div>

<div class="form-group">
<label>Chunk Size (bytes, for chunked mode):</label>
<input type="number" name="chunksize" id="chunksize" value="20" min="1" max="512">
</div>

<div class="form-group">
<label>Delay Between Chunks (ms):</label>
<input type="number" name="chunkdelay" id="chunkdelay" value="2" min="0" max="1000">
</div>

<button type="submit" name="action" value="analyze">📊 Analyze Only</button>
<button type="submit" name="action" value="send">📡 Send to Device</button>
<button type="submit" name="action" value="dump">🔍 Show Protocol Dump</button>
</form>
</div>

<script>
// Save form values to localStorage on change
function saveFormState() {
  localStorage.setItem('giftest_mode', document.getElementById('mode').value);
  localStorage.setItem('giftest_reinit', document.getElementById('reinit').checked);
  localStorage.setItem('giftest_chunksize', document.getElementById('chunksize').value);
  localStorage.setItem('giftest_chunkdelay', document.getElementById('chunkdelay').value);
}

// Restore form values from localStorage on page load
function restoreFormState() {
  const mode = localStorage.getItem('giftest_mode');
  if (mode) document.getElementById('mode').value = mode;
  
  const reinit = localStorage.getItem('giftest_reinit');
  if (reinit !== null) document.getElementById('reinit').checked = reinit === 'true';
  
  const chunksize = localStorage.getItem('giftest_chunksize');
  if (chunksize) document.getElementById('chunksize').value = chunksize;
  
  const chunkdelay = localStorage.getItem('giftest_chunkdelay');
  if (chunkdelay) document.getElementById('chunkdelay').value = chunkdelay;
}

// Add event listeners
document.getElementById('mode').addEventListener('change', saveFormState);
document.getElementById('reinit').addEventListener('change', saveFormState);
document.getElementById('chunksize').addEventListener('input', saveFormState);
document.getElementById('chunkdelay').addEventListener('input', saveFormState);

// Restore state on page load
restoreFormState();
</script>
</div>

<div class="section">
<h2>📝 Bluetooth Dump Analysis</h2>
<p>The <code>lonelygif.txt</code> dump shows:</p>
<ul>
<li>Protocol header <code>88ff</code> (different from our <code>99aa</code>)</li>
<li>Chunked writes with handle 0x0082</li>
<li>Multiple sequential writes for large payloads</li>
<li>Specific timing patterns between chunks</li>
<li><strong>Vendor app ALWAYS sends reset/clear before starting</strong></li>
</ul>
<p><strong>Key Discovery:</strong> The proprietary app uses two protocols - <code>99aa</code> for commands and <code>88ff</code> for data chunks!</p>
</div>

<div class="section">
<h2>🔧 Protocol Investigation Tools</h2>
<ul>
<li><strong>Analyze:</strong> Extract frame count, size, delays from uploaded GIF</li>
<li><strong>Send:</strong> Upload GIF using selected mode and observe behavior</li>
<li><strong>Dump:</strong> Show hex dump of what would be sent to device</li>
</ul>
</div>

</body>
</html>`

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(tmpl))
}

func (h *Handler) processGifTest(w http.ResponseWriter, r *http.Request) {
	// Parse form
	if err := r.ParseMultipartForm(10 << 20); err != nil { // 10MB max
		http.Error(w, "Failed to parse form", http.StatusBadRequest)
		return
	}

	// Get uploaded file
	file, header, err := r.FormFile("gif")
	if err != nil {
		http.Error(w, "No file uploaded", http.StatusBadRequest)
		return
	}
	defer file.Close()

	gifData, err := io.ReadAll(file)
	if err != nil {
		http.Error(w, "Failed to read file", http.StatusInternalServerError)
		return
	}

	// Analyze GIF
	analysis, err := analyzeGif(gifData)
	if err != nil {
		http.Error(w, fmt.Sprintf("Failed to analyze GIF: %v", err), http.StatusBadRequest)
		return
	}

	action := r.FormValue("action")
	mode := r.FormValue("mode")
	reinit := r.FormValue("reinit") == "on"
	chunkSize, _ := strconv.Atoi(r.FormValue("chunksize"))
	if chunkSize == 0 {
		chunkSize = 20
	}
	_ = chunkSize // Reserved for future chunked mode implementation

	// Build response page
	var response strings.Builder
	response.WriteString(`<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Test Results - XYAOLED GIF Test</title>
<style>
body { font-family: system-ui, sans-serif; max-width: 1200px; margin: 20px auto; padding: 0 20px; background: #0a0a0a; color: #e0e0e0; }
h1 { color: #4CAF50; }
.result { margin: 20px 0; padding: 15px; background: #2a2a2a; border-radius: 4px; }
.result.success { border-left: 4px solid #4CAF50; }
.result.error { border-left: 4px solid #ff5722; }
table { width: 100%; border-collapse: collapse; margin: 15px 0; background: #1a1a1a; }
th, td { padding: 10px; text-align: left; border-bottom: 1px solid #333; }
th { background: #2a2a2a; color: #4CAF50; }
.hex-dump { background: #1a1a1a; padding: 15px; border-radius: 4px; overflow-x: auto; font-family: 'Courier New', monospace; font-size: 12px; color: #FFC107; border: 1px solid #333; }
button { padding: 12px 24px; background: #4CAF50; color: white; border: none; border-radius: 4px; cursor: pointer; margin: 10px 5px; }
button:hover { background: #45a049; }
</style>
</head>
<body>
<h1>🔬 Test Results</h1>
`)

	// Show analysis
	response.WriteString(fmt.Sprintf(`
<div class="result success">
<h2>📊 GIF Analysis: %s</h2>
<table>
<tr><th>Property</th><th>Value</th></tr>
<tr><td>Filename</td><td>%s</td></tr>
<tr><td>File Size</td><td>%d bytes (%.2f KB)</td></tr>
<tr><td>Frame Count</td><td>%d frames</td></tr>
<tr><td>Dimensions</td><td>%s</td></tr>
<tr><td>Total Duration</td><td>%d ms (%.2f seconds)</td></tr>
<tr><td>Loop Count</td><td>%d (0 = infinite)</td></tr>
<tr><td>Average Frame Delay</td><td>%.1f ms</td></tr>
</table>
</div>
`, header.Filename, header.Filename, analysis.TotalSize, float64(analysis.TotalSize)/1024.0,
		analysis.FrameCount, analysis.Dimensions, analysis.TotalDuration,
		float64(analysis.TotalDuration)/1000.0, analysis.LoopCount,
		float64(analysis.TotalDuration)/float64(analysis.FrameCount)))

	// Handle different actions
	switch action {
	case "analyze":
		response.WriteString(`<div class="result success"><p>✅ Analysis complete. No send operation performed.</p></div>`)

	case "dump":
		// Show what would be sent
		processed, err := h.processGifForMode(gifData, mode)
		if err != nil {
			response.WriteString(fmt.Sprintf(`<div class="result error"><p>❌ Processing failed: %v</p></div>`, err))
		} else {
			response.WriteString(fmt.Sprintf(`
<div class="result success">
<h2>🔍 Protocol Dump (Mode: %s)</h2>
<p>Processed size: %d bytes (%.2f KB)</p>
<div class="hex-dump">%s</div>
</div>
`, mode, len(processed), float64(len(processed))/1024.0, formatHexDump(processed, 32)))
		}

	case "send":
		// Actually send to device
		if mode == "chunked-88ff" {
			// Use new chunked protocol directly
			err := h.display.ShowAnimatedGifChunked(base64.StdEncoding.EncodeToString(gifData))
			if err != nil {
				response.WriteString(fmt.Sprintf(`<div class="result error"><p>❌ Chunked send failed: %v</p></div>`, err))
			} else {
				response.WriteString(fmt.Sprintf(`
<div class="result success">
<h2>✅ Successfully Sent via Chunked 88ff Protocol!</h2>
<p>Mode: %s | Frames: %d</p>
<p><strong>Check the device - all frames should be displaying!</strong></p>
<p>This protocol supports 18+ frames by chunking data transfer.</p>
</div>
`, mode, analysis.FrameCount))
			}
		} else {
			// Use legacy modes
			processed, err := h.processGifForMode(gifData, mode)
			if err != nil {
				response.WriteString(fmt.Sprintf(`<div class="result error"><p>❌ Processing failed: %v</p></div>`, err))
			} else {
				opts := xyaoled.SendOptions{
					ForceReinit: reinit,
				}

				err = h.display.device.SendGifWithOptions(processed, opts)
				if err != nil {
					response.WriteString(fmt.Sprintf(`<div class="result error"><p>❌ Send failed: %v</p></div>`, err))
				} else {
					response.WriteString(fmt.Sprintf(`
<div class="result success">
<h2>✅ Successfully Sent to Device</h2>
<p>Mode: %s | Reinit: %v | Size: %d bytes</p>
<p>Check the device to see how many frames are displayed!</p>
</div>
`, mode, reinit, len(processed)))
				}
			}
		}
	}

	response.WriteString(`
<button onclick="window.location='/uploadTestGif'">🔙 Back to Test Interface</button>
</body>
</html>`)

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(response.String()))
}

func (h *Handler) processGifForMode(gifData []byte, mode string) ([]byte, error) {
	switch mode {
	case "raw":
		// Send original GIF data without any processing
		return gifData, nil

	case "noresize":
		// Decode and re-encode without resizing
		g, err := gif.DecodeAll(bytes.NewReader(gifData))
		if err != nil {
			return nil, err
		}
		var buf bytes.Buffer
		if err := gif.EncodeAll(&buf, g); err != nil {
			return nil, err
		}
		return buf.Bytes(), nil

	case "standard", "chunked":
		// Use standard processing (resize to display size)
		// This is what our current implementation does
		g, err := gif.DecodeAll(bytes.NewReader(gifData))
		if err != nil {
			return nil, err
		}
		return h.display.processAnimatedGifWithLoop(g, false)

	default:
		return nil, fmt.Errorf("unknown mode: %s", mode)
	}
}

func formatHexDump(data []byte, maxLines int) string {
	var buf strings.Builder
	lineCount := 0
	for i := 0; i < len(data); i += 16 {
		if lineCount >= maxLines {
			buf.WriteString(fmt.Sprintf("\n... (%d more bytes)", len(data)-i))
			break
		}
		end := i + 16
		if end > len(data) {
			end = len(data)
		}

		// Offset
		buf.WriteString(fmt.Sprintf("%04x  ", i))

		// Hex bytes
		for j := i; j < end; j++ {
			buf.WriteString(fmt.Sprintf("%02x ", data[j]))
		}

		// Padding
		for j := end; j < i+16; j++ {
			buf.WriteString("   ")
		}

		// ASCII
		buf.WriteString(" |")
		for j := i; j < end; j++ {
			if data[j] >= 32 && data[j] <= 126 {
				buf.WriteByte(data[j])
			} else {
				buf.WriteByte('.')
			}
		}
		buf.WriteString("|\n")
		lineCount++
	}
	return buf.String()
}

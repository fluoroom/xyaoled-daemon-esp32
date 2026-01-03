# GIF Frame Analysis - Experimental Test Interface

## 🎯 Purpose

Investigate why the proprietary app successfully sends large multi-frame GIFs (20+ frames) while our implementation only displays 6 frames from an 18-frame GIF.

## 📊 Key Findings from lonelygif.txt

The Wireshark bluetooth dump (`lonelygif.txt`) shows the proprietary app using:

### Protocol Headers
- **99aa** - Standard protocol header (only 4 occurrences)
- **88ff** - Extended/sequenced protocol (many occurrences)

### Write Pattern
```
Frame 41896: 99aa002eff881f00000001001a0103102c3b0601006aa968be268100000000 (init command)
Subsequent: 88ff030000..., 88ff030100..., 88ff030200... (sequenced data)
```

### Bluetooth Details
- **Characteristic Handle**: 0x0082 (UUID ae01)
- **Connection Handle**: 0x0004  
- **Device**: XyaoLED_AE16 (MAC: 2a:4d:fd:ab:ae:16)
- **Write Type**: ATT Write Commands (no response required)

## 🔬 Test Interface

Access the experimental test interface at: **http://localhost:5000/uploadTestGif**

### Features

1. **Analyze Mode**: Extract frame count, duration, and metadata from uploaded GIF
2. **Send Mode**: Upload GIF using different processing methods
3. **Dump Mode**: Show hex dump of what would be sent to device

### Test Modes

- **Standard**: Current implementation (resize to display size)
- **Raw**: Send original GIF data without processing
- **No Resize**: Decode and re-encode without resizing
- **Chunked**: Test paced upload (planned for 88ff protocol)

### Configuration Options

- **Force Reinit**: Clear device state before send
- **Chunk Size**: Bytes per chunk (for chunked mode)
- **Chunk Delay**: Milliseconds between chunks

## 📝 Analysis Results

The interface displays:
- Frame count
- Total file size  
- Image dimensions
- Total animation duration
- Loop count
- Average frame delay

## 🔍 Investigation Plan

1. **Extract Protocol Sequence** from lonelygif.txt
   ```fish
   grep "Value: " /home/nic0/projects/xyaoled-daemon/lonelygif.txt > extracted_writes.txt
   ```

2. **Analyze 88ff Protocol Structure**
   - Determine sequence number format
   - Identify chunk size patterns
   - Compare timing between writes

3. **Test Different Frame Counts**
   - 6 frames (known to work)
   - 18 frames (only 6 display)
   - 20+ frames (like vendor app)

4. **Implement 88ff Protocol** if needed
   - Add sequenced write support to xyaoled/device.go
   - Test chunked transfer for large GIF data

## 💡 Hypotheses

### Why Only 6 Frames Display?

1. **Device Buffer Limit**: Device may have fixed frame buffer (6 frames max)
2. **Protocol Limitation**: Standard 99aa protocol may not support 18+ frames
3. **Processing Truncation**: Our GIF processing may be limiting frames
4. **Transfer Size**: Single write may be too large, need chunked transfer via 88ff

### Vendor App Advantage

- Uses 88ff extended protocol for large data
- Chunks data into sequential writes
- May use different encoding/compression
- Might send frame data differently than full GIF

## 🚀 Next Steps

1. Extract all `88ff` writes from lonelygif.txt
2. Decode the sequence pattern and data structure
3. Test GIFs with 6, 12, 18, 24 frames using the interface
4. Compare behavior: standard protocol vs our protocol
5. Implement 88ff protocol support if confirmed necessary

## 📂 Related Files

- `experimental.go` - Test interface implementation
- `lonelygif.txt` - Bluetooth dump (2.5M lines, ~50MB)
- `display.go` - Current GIF processing logic
- `xyaoled/device.go` - BLE communication layer

## 🔗 Endpoints

- `GET /uploadTestGif` - Web interface
- `POST /uploadTestGif?action=analyze` - Analyze GIF
- `POST /uploadTestGif?action=send` - Send to device
- `POST /uploadTestGif?action=dump` - Show protocol dump

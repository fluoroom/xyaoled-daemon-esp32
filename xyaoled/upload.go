package xyaoled

// Acknowledged upload protocol, recovered from the vendor app (Xyao LED 1.6.0,
// libapp.so sha256 f24522449b15...) with REA's Ghidra provider. Addresses are
// Ghidra addresses (ELF vaddr + 0x100000) in that build.
//
//   - Uploads are split into packets of at most 10000 bytes, each a complete
//     99aa002eff88 frame (0x855230: block size 0x2710, header 24 bytes for the
//     first packet and 16 for the rest).
//   - After each packet the device answers on ae02 with
//     88 ff 03 <idxLo> <idxHi> <ctr> .. .. <status> ..   (0x853ed8..0x8540d0)
//     ctr de-duplicates repeated notifications, status >= 0x14 means OK,
//     anything lower is a rejection that aborts the transfer (0x54ca74).
//   - The next packet is only sent after the ack (0x8588e0). If no ack
//     arrives the same packet is re-sent from a timer (0x871d8c, 0x5548bc).

import (
	"errors"
	"fmt"
	"time"
)

const (
	maxPacketSize    = 10000
	firstHeaderSize  = 24
	nextHeaderSize   = 16
	packetTypeFirst  = 0x07
	packetTypeMiddle = 0x08
	packetTypeLast   = 0x09
	ackStatusMinOK   = 0x14
	maxPacketResends = 3

	// After an upload the panel is busy for a while (tens of seconds after a
	// large GIF) and ignores the program select; it is re-sent until accepted.
	selectRetryInterval = time.Second
	panelBusyTimeout    = 60 * time.Second

	// uploadAppliedTimeout bounds the wait for 88ff05 after an upload.
	uploadAppliedTimeout = 5 * time.Second

	// clearPlaylistSettleDelay gives the panel time to apply a playlist clear
	// before the next command lands; the clear sends no ack to wait on.
	clearPlaylistSettleDelay = 500 * time.Millisecond
)

var (
	errNoAck = errors.New("no ack from device")
	// errNotReady means the panel answered data with 88ff05: it is not in
	// receive mode, so the upload must restart from the program select.
	errNotReady = errors.New("panel is not accepting data")
	// ErrPanelBusy means the panel never accepted the program select.
	ErrPanelBusy = errors.New("panel busy: program select not acknowledged")
)

// isNotReady reports whether b is an 88ff05 notification.
func isNotReady(b []byte) bool {
	return len(b) >= 3 && b[0] == 0x88 && b[1] == 0xff && b[2] == 0x05
}

// ack is a parsed 88ff03 transfer acknowledgement.
type ack struct {
	index  uint16
	ctr    byte
	status byte
}

func parseAck(b []byte) (ack, bool) {
	if len(b) < 9 || b[0] != 0x88 || b[1] != 0xff || b[2] != 0x03 {
		return ack{}, false
	}
	return ack{index: uint16(b[3]) | uint16(b[4])<<8, ctr: b[5], status: b[8]}, true
}

// buildUploadPackets splits payload into acknowledged packets for program
// with command sequence number seq. trailer is the 8 header bytes that follow
// the packet count in the first packet.
func buildUploadPackets(payload []byte, program, seq byte, trailer [8]byte) [][]byte {
	firstCap := maxPacketSize - firstHeaderSize
	nextCap := maxPacketSize - nextHeaderSize
	n := 1
	if len(payload) > firstCap {
		n += (len(payload) - firstCap + nextCap - 1) / nextCap
	}

	packets := make([][]byte, 0, n)
	offset := 0
	for i := 0; i < n; i++ {
		remaining := len(payload) - offset
		headerSize, capacity := nextHeaderSize, nextCap
		if i == 0 {
			headerSize, capacity = firstHeaderSize, firstCap
		}
		chunk := payload[offset : offset+min(remaining, capacity)]

		typ := byte(packetTypeMiddle)
		switch {
		case i == 0:
			typ = packetTypeFirst
		case i == n-1:
			typ = packetTypeLast
		}

		total := headerSize + len(chunk)
		p := make([]byte, 0, total)
		p = append(p, 0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88)
		p = append(p, byte(total), byte(total>>8))
		p = append(p, typ, program, seq)
		p = append(p, byte(remaining), byte(remaining>>8), byte(remaining>>16))
		if i == 0 {
			count := n + 1
			p = append(p, byte(count), byte(count>>8))
			p = append(p, trailer[:]...)
		} else {
			p = append(p, byte(i+1), byte((i+1)>>8))
		}
		p = append(p, chunk...)
		packets = append(packets, p)
		offset += len(chunk)
	}
	return packets
}

// ackTimeout mirrors the vendor's per-packet timer: 200ms per 250 payload
// bytes plus 200ms.
func ackTimeout(packetLen int) time.Duration {
	return time.Duration((packetLen+249)/250*200+200) * time.Millisecond
}

// writePacket writes one protocol frame, split into ATT-sized writes.
func (d *Device) writePacket(p []byte) error {
	chunk := d.attPayloadSize()
	for off := 0; off < len(p); off += chunk {
		if err := d.send(p[off:min(off+chunk, len(p))]); err != nil {
			return err
		}
	}
	return nil
}

// attPayloadSize returns the largest single write BlueZ accepts (MTU-3).
func (d *Device) attPayloadSize() int {
	if mtu, err := d.writeChar.GetMTU(); err == nil && mtu > 3 {
		return int(mtu) - 3
	}
	return 20
}

// drainNotifications drops stale notifications so an old ack is never
// mistaken for the answer to the next packet.
func (d *Device) drainNotifications() {
	for {
		select {
		case <-d.notifyChan:
		default:
			return
		}
	}
}

// waitAck waits for the ack with index want (0 acks the program select, n acks
// packet n). Duplicates and acks for other indices, such as late acks for the
// program select, are skipped. While waiting for a packet ack, an 88ff05
// notification returns errNotReady.
func (d *Device) waitAck(want uint16, timeout time.Duration) (ack, error) {
	deadline := time.After(timeout)
	for {
		select {
		case buf := <-d.notifyChan:
			if want > 0 && isNotReady(buf) {
				return ack{}, errNotReady
			}
			a, ok := parseAck(buf)
			if !ok {
				continue
			}
			if d.haveLastAckCtr && a.ctr == d.lastAckCtr {
				continue
			}
			d.lastAckCtr, d.haveLastAckCtr = a.ctr, true
			if a.index != want {
				continue
			}
			return a, nil
		case <-deadline:
			return ack{}, errNoAck
		}
	}
}

// waitUploadApplied waits for the 88ff05 notification the panel sends once it
// has taken in a finished upload. Commands sent before it arrives (such as
// the reinit handshake) can be rejected, after which the panel disconnects.
func (d *Device) waitUploadApplied() {
	start := time.Now()
	deadline := time.After(uploadAppliedTimeout)
	for {
		select {
		case buf := <-d.notifyChan:
			if isNotReady(buf) {
				if d.debug {
					fmt.Printf("[BLE] Upload applied after %v\n", time.Since(start).Round(time.Millisecond))
				}
				return
			}
		case <-deadline:
			if d.debug {
				fmt.Printf("[BLE] No upload-applied notification after %v\n", uploadAppliedTimeout)
			}
			return
		}
	}
}

// selectProgram sends the program select command until the panel acks it,
// returning ErrPanelBusy if it never does within panelBusyTimeout.
func (d *Device) selectProgram(program, seq byte) error {
	cmd := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0d, 0x00, 0x04, program, seq, 0xff, 0x00}
	start := time.Now()
	for {
		d.drainNotifications()
		if err := d.writePacket(cmd); err != nil {
			return err
		}
		if _, err := d.waitAck(0, selectRetryInterval); err == nil {
			return nil
		}
		if time.Since(start) > panelBusyTimeout {
			return ErrPanelBusy
		}
		if d.debug {
			fmt.Printf("[BLE] Panel busy (%v), re-sending program select\n", time.Since(start).Round(time.Second))
		}
	}
}

// uploadPackets sends packets one at a time, waiting for the device to ack
// each before sending the next. Unacknowledged packets are re-sent.
func (d *Device) uploadPackets(packets [][]byte) error {
	for i, p := range packets {
		want := uint16(i + 1)
		var a ack
		var err error
		for attempt := 0; attempt <= maxPacketResends; attempt++ {
			d.drainNotifications()
			if d.debug {
				fmt.Printf("[BLE] Packet %d/%d (%d bytes), attempt %d\n", i+1, len(packets), len(p), attempt+1)
			}
			if err = d.writePacket(p); err != nil {
				return fmt.Errorf("failed to write packet %d/%d: %w", i+1, len(packets), err)
			}
			a, err = d.waitAck(want, ackTimeout(len(p)))
			if err != errNoAck {
				break
			}
		}
		if err != nil {
			return fmt.Errorf("packet %d/%d: %w", i+1, len(packets), err)
		}
		if a.status < ackStatusMinOK {
			return fmt.Errorf("device rejected packet %d/%d (status 0x%02x)", i+1, len(packets), a.status)
		}
	}
	return nil
}

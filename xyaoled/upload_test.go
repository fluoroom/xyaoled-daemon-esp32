package xyaoled

import (
	"bytes"
	"testing"
	"time"
)

var testTrailer = [8]byte{0x00, 0x01, 0x20, 0x03, 0x06, 0x00, 0x01, 0x00}

func TestBuildUploadPacketsSingle(t *testing.T) {
	payload := bytes.Repeat([]byte{0xab}, 1949)
	packets := buildUploadPackets(payload, 0x02, 0x04, testTrailer)
	if len(packets) != 1 {
		t.Fatalf("got %d packets, want 1", len(packets))
	}
	p := packets[0]
	want := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0xb5, 0x07, 0x07, 0x02, 0x04, 0x9d, 0x07, 0x00, 0x02, 0x00}
	if !bytes.Equal(p[:16], want) {
		t.Errorf("header = % x, want % x", p[:16], want)
	}
	if !bytes.Equal(p[16:24], testTrailer[:]) {
		t.Errorf("trailer = % x", p[16:24])
	}
	if !bytes.Equal(p[24:], payload) {
		t.Error("payload not copied after 24-byte header")
	}
}

func TestBuildUploadPacketsMulti(t *testing.T) {
	payload := make([]byte, 94445)
	for i := range payload {
		payload[i] = byte(i)
	}
	packets := buildUploadPackets(payload, 0x03, 0x04, testTrailer)
	if len(packets) != 10 {
		t.Fatalf("got %d packets, want 10", len(packets))
	}

	var joined []byte
	remaining := len(payload)
	for i, p := range packets {
		if len(p) > maxPacketSize {
			t.Errorf("packet %d is %d bytes", i, len(p))
		}
		if got := int(p[6]) | int(p[7])<<8; got != len(p) {
			t.Errorf("packet %d length field %d, want %d", i, got, len(p))
		}
		wantType := byte(packetTypeMiddle)
		if i == 0 {
			wantType = packetTypeFirst
		} else if i == len(packets)-1 {
			wantType = packetTypeLast
		}
		if p[8] != wantType || p[9] != 0x03 || p[10] != 0x04 {
			t.Errorf("packet %d type/seq/slot = % x", i, p[8:11])
		}
		if got := int(p[11]) | int(p[12])<<8 | int(p[13])<<16; got != remaining {
			t.Errorf("packet %d remaining field %d, want %d", i, got, remaining)
		}
		header := nextHeaderSize
		if i == 0 {
			header = firstHeaderSize
			if got := int(p[14]) | int(p[15])<<8; got != len(packets)+1 {
				t.Errorf("count field %d, want %d", got, len(packets)+1)
			}
		} else if got := int(p[14]) | int(p[15])<<8; got != i+1 {
			t.Errorf("packet %d index field %d, want %d", i, got, i+1)
		}
		joined = append(joined, p[header:]...)
		remaining -= len(p) - header
	}
	if !bytes.Equal(joined, payload) {
		t.Error("payload does not survive split")
	}
	if len(packets[9]) != 4613 {
		t.Errorf("last packet %d bytes, want 4613", len(packets[9]))
	}
}

func TestParseAck(t *testing.T) {
	a, ok := parseAck([]byte{0x88, 0xff, 0x03, 0x01, 0x00, 0xc2, 0x0b, 0x00, 0xff, 0x0b, 0x62})
	if !ok || a.index != 1 || a.ctr != 0xc2 || a.status != 0xff {
		t.Errorf("parseAck = %+v, %v", a, ok)
	}
	if _, ok := parseAck([]byte{0x88, 0xff, 0x00, 0x02, 0x01}); ok {
		t.Error("non-03 notification parsed as ack")
	}
}

func TestAckTimeout(t *testing.T) {
	if got := ackTimeout(10000); got != 8200*time.Millisecond {
		t.Errorf("ackTimeout(10000) = %v", got)
	}
	if got := ackTimeout(13); got != 400*time.Millisecond {
		t.Errorf("ackTimeout(13) = %v", got)
	}
}

package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"go.bug.st/serial"
)

// WAVHeader represents the WAV file header structure
type WAVHeader struct {
	RiffHeader    [4]byte
	FileSize      uint32
	WaveHeader    [4]byte
	FmtHeader     [4]byte
	FmtSize       uint32
	AudioFormat   uint16
	NumChannels   uint16
	SampleRate    uint32
	ByteRate      uint32
	BlockAlign    uint16
	BitsPerSample uint16
	DataHeader    [4]byte
	DataSize      uint32
}

// PCMToWAVConverter captures PCM audio from serial port and saves as WAV files
type PCMToWAVConverter struct {
	port                    string
	baudrate                int
	sampleRate              uint32
	channels                uint16
	sampleWidth             uint16
	durationSecs            int
	outputDir               string
	bytesPerFile            int
	serialPort              serial.Port
	running                 bool
	fileCount               int
	debug                   bool
	bytesReadSinceLastPrint int
	lastPrintTime           time.Time
}

// NewPCMToWAVConverter creates a new converter instance
func NewPCMToWAVConverter(port string, baudrate int, sampleRate uint32, channels uint16,
	sampleWidth uint16, durationSecs int, outputDir string, debug bool) *PCMToWAVConverter {

	bytesPerFile := int(sampleRate) * durationSecs * int(channels) * int(sampleWidth)

	converter := &PCMToWAVConverter{
		port:          port,
		baudrate:      baudrate,
		sampleRate:    sampleRate,
		channels:      channels,
		sampleWidth:   sampleWidth,
		durationSecs:  durationSecs,
		outputDir:     outputDir,
		bytesPerFile:  bytesPerFile,
		debug:         debug,
		lastPrintTime: time.Now(),
	}

	converter.printConfig()
	return converter
}

// printConfig prints the configuration
func (c *PCMToWAVConverter) printConfig() {
	requiredBytesPerSec := int(c.sampleRate) * int(c.channels) * int(c.sampleWidth)
	availableBytesPerSec := c.baudrate / 8
	bandwidthPercent := float64(requiredBytesPerSec*100) / float64(availableBytesPerSec)

	fmt.Printf("PCM Reader Configuration:\n")
	fmt.Printf("  Port: %s\n", c.port)
	fmt.Printf("  Baudrate: %d\n", c.baudrate)
	fmt.Printf("  Sample Rate: %d Hz\n", c.sampleRate)
	fmt.Printf("  Channels: %d\n", c.channels)
	fmt.Printf("  Sample Width: %d bytes\n", c.sampleWidth)
	fmt.Printf("  Duration per file: %d seconds\n", c.durationSecs)
	fmt.Printf("  Bytes per file: %d\n", c.bytesPerFile)
	fmt.Printf("  Output directory: %s\n", c.outputDir)
	fmt.Printf("\n")
	fmt.Printf("Bandwidth Analysis:\n")
	fmt.Printf("  Required data rate: %d bytes/sec\n", requiredBytesPerSec)
	fmt.Printf("  Available at %d baud: %d bytes/sec\n", c.baudrate, availableBytesPerSec)
	fmt.Printf("  Bandwidth usage: %.1f%%\n", bandwidthPercent)

	if bandwidthPercent > 90 {
		fmt.Printf("\n⚠️  WARNING: Bandwidth usage is %.1f%% - INCREASE BAUD RATE!\n", bandwidthPercent)
		fmt.Printf("    Recommended: 460800 or 921600 baud\n\n")
	}
	fmt.Println()
}

// Connect establishes serial connection
func (c *PCMToWAVConverter) Connect() error {
	mode := &serial.Mode{
		BaudRate: c.baudrate,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}

	port, err := serial.Open(c.port, mode)
	if err != nil {
		return fmt.Errorf("failed to connect to %s: %w", c.port, err)
	}

	// Set read timeout to prevent blocking indefinitely
	port.SetReadTimeout(100 * time.Millisecond)

	c.serialPort = port
	fmt.Printf("✓ Connected to %s at %d baud\n", c.port, c.baudrate)
	return nil
}

// Disconnect closes the serial connection
func (c *PCMToWAVConverter) Disconnect() {
	if c.serialPort != nil {
		c.serialPort.Close()
		fmt.Println("✓ Disconnected from serial port")
	}
}

// WriteWAVFile writes PCM data to a WAV file
func (c *PCMToWAVConverter) WriteWAVFile(pcmData []byte) error {
	// Create output directory if it doesn't exist
	if err := os.MkdirAll(c.outputDir, 0755); err != nil {
		return fmt.Errorf("failed to create output directory: %w", err)
	}

	// Generate filename with timestamp
	timestamp := time.Now().Format("20060102_150405")
	filename := filepath.Join(c.outputDir, fmt.Sprintf("audio_%s_%04d.wav", timestamp, c.fileCount))

	// Create WAV header
	bytesPerSecond := uint32(int(c.sampleRate) * int(c.channels) * int(c.sampleWidth))
	blockAlign := uint16(int(c.channels) * int(c.sampleWidth))
	bitsPerSample := c.sampleWidth * 8
	fileSize := uint32(36 + len(pcmData))

	header := WAVHeader{
		RiffHeader:    [4]byte{'R', 'I', 'F', 'F'},
		FileSize:      fileSize,
		WaveHeader:    [4]byte{'W', 'A', 'V', 'E'},
		FmtHeader:     [4]byte{'f', 'm', 't', ' '},
		FmtSize:       16,
		AudioFormat:   1, // PCM
		NumChannels:   c.channels,
		SampleRate:    c.sampleRate,
		ByteRate:      bytesPerSecond,
		BlockAlign:    blockAlign,
		BitsPerSample: bitsPerSample,
		DataHeader:    [4]byte{'d', 'a', 't', 'a'},
		DataSize:      uint32(len(pcmData)),
	}

	// Write WAV file
	file, err := os.Create(filename)
	if err != nil {
		return fmt.Errorf("failed to create WAV file: %w", err)
	}
	defer file.Close()

	// Write header
	if err := binary.Write(file, binary.LittleEndian, &header); err != nil {
		return fmt.Errorf("failed to write WAV header: %w", err)
	}

	// Write PCM data
	if _, err := file.Write(pcmData); err != nil {
		return fmt.Errorf("failed to write PCM data: %w", err)
	}

	c.fileCount++
	fileSizeKB := float64(len(pcmData)) / 1024.0
	fmt.Printf("✓ Written %s (%.1f KB)\n", filepath.Base(filename), fileSizeKB)

	return nil
}

// ReadAndConvert reads PCM data from serial and converts to WAV files
func (c *PCMToWAVConverter) ReadAndConvert() {
	if err := c.Connect(); err != nil {
		fmt.Printf("✗ %v\n", err)
		return
	}

	defer c.Disconnect()

	// Setup signal handler for graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	c.running = true
	buffer := make([]byte, 0, c.bytesPerFile)

	fmt.Println("Starting to read PCM data... (Ctrl+C to stop)")
	fmt.Println()

	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()

	for c.running {
		select {
		case <-sigChan:
			fmt.Printf("\n\n✓ Interrupted by user\n")
			if len(buffer) > 0 {
				fmt.Printf("Saving partial buffer (%d bytes)...\n", len(buffer))
				if err := c.WriteWAVFile(buffer); err != nil {
					fmt.Printf("✗ Error writing buffer: %v\n", err)
				}
			}
			c.running = false
			continue

		case <-ticker.C:
			// Read available data with larger buffer
			readBuf := make([]byte, 8192)
			n, err := c.serialPort.Read(readBuf)

			if err != nil && err != io.EOF {
				fmt.Printf("\n✗ Serial read error: %v\n", err)
				c.running = false
				break
			}

			if n > 0 {
				c.bytesReadSinceLastPrint += n
				buffer = append(buffer, readBuf[:n]...)

				// Show progress
				progress := float64(len(buffer)) * 100.0 / float64(c.bytesPerFile)
				now := time.Now()
				elapsed := now.Sub(c.lastPrintTime).Seconds()

				if elapsed > 1.0 {
					if c.debug {
						bytesPerSec := float64(c.bytesReadSinceLastPrint) / elapsed
						reqBytesPerSec := float64(c.sampleRate) * float64(c.channels) * float64(c.sampleWidth)
						fmt.Printf("\rProgress: %.1f%% | Actual: %.0f bytes/sec | Required: %.0f bytes/sec | Buffer: %d/%d bytes",
							progress, bytesPerSec, reqBytesPerSec, len(buffer), c.bytesPerFile)
					} else {
						fmt.Printf("\rProgress: %.1f%% (%d/%d bytes)",
							progress, len(buffer), c.bytesPerFile)
					}
					c.bytesReadSinceLastPrint = 0
					c.lastPrintTime = now
				}

				// Check if we have enough data for a complete file
				if len(buffer) >= c.bytesPerFile {
					// Extract one file's worth of data
					pcmData := make([]byte, c.bytesPerFile)
					copy(pcmData, buffer[:c.bytesPerFile])

					// Remove written data from buffer
					buffer = buffer[c.bytesPerFile:]

					// Write to WAV file
					fmt.Println() // Newline after progress indicator
					if err := c.WriteWAVFile(pcmData); err != nil {
						fmt.Printf("✗ Error writing WAV file: %v\n", err)
					}
				}
			}
		}
	}

	fmt.Printf("\nTotal files written: %d\n", c.fileCount)
}

// parseArgs parses command-line arguments
type Args struct {
	Port        string
	Baudrate    int
	SampleRate  int
	Channels    int
	SampleWidth int
	Duration    int
	OutputDir   string
	Debug       bool
}

func parseArgs() Args {
	args := Args{}

	flag.StringVar(&args.Port, "port", "/dev/ttyUSB0",
		"Serial port device")
	flag.IntVar(&args.Baudrate, "baudrate", 921600,
		"Serial baud rate")
	flag.IntVar(&args.SampleRate, "sample-rate", 16000,
		"Audio sample rate in Hz")
	flag.IntVar(&args.Channels, "channels", 1,
		"Number of audio channels")
	flag.IntVar(&args.SampleWidth, "sample-width", 2,
		"Bytes per sample: 1=8-bit, 2=16-bit, 4=32-bit")
	flag.IntVar(&args.Duration, "duration", 20,
		"Duration of each WAV file in seconds")
	flag.StringVar(&args.OutputDir, "output-dir", "./audio_captures",
		"Output directory for WAV files")
	flag.BoolVar(&args.Debug, "debug", false,
		"Enable debug output with bandwidth statistics")

	flag.Parse()

	return args
}

// validateArgs validates the parsed arguments
func validateArgs(args Args) error {
	if args.Baudrate <= 0 {
		return fmt.Errorf("error: baud rate must be positive")
	}
	if args.SampleRate <= 0 {
		return fmt.Errorf("error: sample rate must be positive")
	}
	if args.Channels <= 0 {
		return fmt.Errorf("error: number of channels must be positive")
	}
	if args.Duration <= 0 {
		return fmt.Errorf("error: duration must be positive")
	}
	if args.SampleWidth != 1 && args.SampleWidth != 2 && args.SampleWidth != 4 {
		return fmt.Errorf("error: sample width must be 1, 2, or 4")
	}
	return nil
}

func main() {
	args := parseArgs()

	if err := validateArgs(args); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	converter := NewPCMToWAVConverter(
		args.Port,
		args.Baudrate,
		uint32(args.SampleRate),
		uint16(args.Channels),
		uint16(args.SampleWidth),
		args.Duration,
		args.OutputDir,
		args.Debug,
	)

	converter.ReadAndConvert()
}

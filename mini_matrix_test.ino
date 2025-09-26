// MIFARE Ultralight Tag Reader with LED Matrix Display
// Reads MIFARE Ultralight tags and displays patterns based on tag data

#include <Adafruit_IS31FL3741.h>
#include <MFRC522.h>
#include <SPI.h>
#include <math.h>

Adafruit_IS31FL3741_QT matrix;
// If colors appear wrong on matrix, try invoking constructor like so:
// Adafruit_IS31FL3741_QT matrix(IS3741_RBG);

// Some boards have just one I2C interface, but some have more...
TwoWire *i2c = &Wire1; // e.g. change this to &Wire1 for QT Py RP2040

// RFID Reader pins
const int SS_PIN = A1;  // RC522 SDA/SS -> QT Py A1
const int RST_PIN = A2; // RC522 RST    -> QT Py A2

MFRC522 mfrc522(SS_PIN, RST_PIN);

byte current_uid[10]; // Max UID size for MFRC522 is 10
byte current_uid_size = 0;
bool pattern_dirty = true;    // Flag to trigger pre-calculation
float static_distances[7][5]; // Cache for pre-calculated distances
uint32_t cached_palette[5];   // Cache for the palette
byte cached_shape_byte;       // Cached UID bytes
byte cached_style_byte;
byte cached_sparsity_byte;
byte cached_pattern_seed_byte;
float cached_ring_size_modifier;

// Frame buffer to reduce flickering
uint32_t frame_buffer[13 * 9]; // 13x9 matrix
bool frame_ready = false;

void setup() {
  // Serial.begin(115200);
  // Serial.println("MIFARE Ultralight Tag Reader");

  // Initialize RFID reader
  SPI.begin();
  mfrc522.PCD_Init();
  // Serial.println("RC522 ready. Tap a MIFARE Ultralight tag...");

  // Initialize LED matrix
  if (!matrix.begin(IS3741_ADDR_DEFAULT, i2c)) {
    // Serial.println("IS41 not found");
    while (1)
      ;
  }

  // Serial.println("IS41 found!");

  // By default the LED controller communicates over I2C at 400 KHz.
  // Arduino Uno can usually do 800 KHz, and 32-bit microcontrollers 1 MHz.
  i2c->setClock(800000);

  // Set brightness to max and bring controller out of shutdown state
  matrix.setLEDscaling(0xFF);
  matrix.setGlobalCurrent(0xFF);
  // Serial.print("Global current set to: ");
  // Serial.println(matrix.getGlobalCurrent());

  // Clear display and keep it blank
  matrix.fill(0);
  matrix.enable(true); // bring out of shutdown
  matrix.setRotation(0);
  matrix.setTextWrap(false);
}

void loop() {
  // Check for new RFID cards
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    // Serial.println("Tag detected!");
    readMifareUltralightTag();
  }

  // If we have a UID, keep animating the pattern
  if (current_uid_size > 0) {
    updateAnimation();
  }

  // Only update display when we have a complete frame ready
  if (frame_ready) {
    matrix.show();
    frame_ready = false;
  }

  // delay(10); // A small delay to control animation speed
}

void readMifareUltralightTag() {
  // Serial.println("Reading MIFARE Ultralight tag...");

  // Check if tag has valid UID
  if (mfrc522.uid.size == 0 || mfrc522.uid.uidByte == NULL) {
    // Serial.println("Invalid tag read - no valid UID data");
    return;
  }

  // Print UID to serial in hex format
  // Serial.print("UID: ");
  // for (byte i = 0; i < mfrc522.uid.size; i++) {
  //   if (mfrc522.uid.uidByte[i] < 0x10)
  //     Serial.print("0");
  //   Serial.print(mfrc522.uid.uidByte[i], HEX);
  //   if (i < mfrc522.uid.size - 1)
  //     Serial.print(" ");
  // }
  // Serial.println();

  // Check if this is a MIFARE Ultralight tag
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  if (piccType != MFRC522::PICC_TYPE_MIFARE_UL) {
    // Serial.println("Not a MIFARE Ultralight tag!");
    return;
  }

  // Serial.println("MIFARE Ultralight tag confirmed!");

  // Read MIFARE Ultralight pages (0-15)
  byte ultralightData[64]; // 16 pages * 4 bytes per page
  bool readSuccess = true;

  for (byte page = 0; page < 16; page++) {
    byte buffer[18];
    byte bufferSize = sizeof(buffer);

    MFRC522::StatusCode status = mfrc522.MIFARE_Read(page, buffer, &bufferSize);
    if (status != MFRC522::STATUS_OK) {
      // Serial.print("Failed to read page ");
      // Serial.print(page);
      // Serial.print(" - Status: ");
      // Serial.println(mfrc522.GetStatusCodeName(status));
      readSuccess = false;
      break;
    }

    // Copy the 4 data bytes (skip the first 2 bytes which are status)
    for (byte i = 0; i < 4; i++) {
      ultralightData[page * 4 + i] = buffer[i + 2];
    }
  }

  if (readSuccess) {
    // Serial.println("Successfully read all pages!");

    // Print the data in a readable format
    // Serial.println("MIFARE Ultralight Data:");
    // for (byte page = 0; page < 16; page++) {
    //   Serial.print("Page ");
    //   if (page < 10)
    //     Serial.print(" ");
    //   Serial.print(page);
    //   Serial.print(": ");

    //   for (byte i = 0; i < 4; i++) {
    //     byte data = ultralightData[page * 4 + i];
    //     if (data < 0x10)
    //       Serial.print("0");
    //     Serial.print(data, HEX);
    //     Serial.print(" ");
    //   }
    //   Serial.println();
    // }

    // Store UID for animation in loop()
    memcpy(current_uid, mfrc522.uid.uidByte, mfrc522.uid.size);
    current_uid_size = mfrc522.uid.size;
    pattern_dirty = true; // Mark pattern for pre-calculation
    // Serial.println("UID stored for new pattern animation");

  } else {
    // Serial.println("Failed to read MIFARE Ultralight data");
    current_uid_size = 0; // Stop any animation
    displayErrorPattern();
  }

  // Halt PICC
  mfrc522.PICC_HaltA();
  // Stop encryption on PCD
  mfrc522.PCD_StopCrypto1();
}

void generatePalette(byte r_seed, byte g_seed, byte b_seed, uint32_t *palette,
                     int palette_size) {
  // Use seeds to generate a color palette
  // Use HSV color space for more vibrant and harmonious colors
  uint16_t base_hue = (r_seed << 8 | g_seed) % 360; // Base hue from 2 bytes
  uint8_t base_saturation = 200 + (b_seed % 56);    // High saturation
  uint8_t base_value = 200 + (g_seed % 56);         // High brightness

  for (int i = 0; i < palette_size; i++) {
    uint16_t hue = (base_hue + i * (360 / palette_size)) % 360;
    uint8_t saturation = base_saturation - (i * 10);
    uint8_t value = base_value - (i * 10);
    palette[i] = matrix.ColorHSV(hue * (65536 / 360), saturation, value);
  }
}

void precalculatePattern(byte uid[], byte uidSize) {
  if (uidSize < 7) {
    return;
  }

  // Generate and cache the color palette
  generatePalette(uid[0], uid[1], uid[2], cached_palette, 5);

  // Cache UID bytes for variety
  cached_shape_byte = uid[3];
  cached_style_byte = uid[4];
  cached_sparsity_byte = uid[5];
  cached_pattern_seed_byte = uid[6];

  // Cache ring size modifier
  cached_ring_size_modifier = 1.0 + ((cached_shape_byte >> 4) & 0x03) * 0.15;

  int centerX = 6;
  int centerY = 4;
  int shape_type = cached_shape_byte % 3;

  // Pre-calculate static distances for each pixel in the top-left quadrant
  for (int y = 0; y <= centerY; y++) {
    for (int x = 0; x <= centerX; x++) {
      if (x == centerX && y == centerY) {
        static_distances[x][y] = 0;
        continue;
      }

      int dx = abs(x - centerX);
      int dy = abs(y - centerY);
      float d;

      // --- 1. Shape Selection ---
      if (shape_type == 0) {
        d = dx + dy;
      } else if (shape_type == 1) {
        d = sqrt(dx * dx + dy * dy);
      } else {
        d = max(dx, dy);
      }

      // --- 2. Shape Variety Modifiers ---
      float twist_factor = (cached_style_byte & 0x03) * 0.05;
      if (twist_factor > 0 && (dx + dy > 0)) {
        float angle = atan2(dy, dx);
        d += twist_factor * angle * 2;
      }

      float pinch_factor =
          (((int8_t)((cached_style_byte >> 2) & 0x03)) - 1.5) * 0.2;
      if (pinch_factor != 0 && (dx + dy > 0)) {
        float diagonal_influence = 2.0 * dx * dy / (dx * dx + dy * dy);
        d -= pinch_factor * diagonal_influence * d;
      }

      float jagged_strength = ((cached_style_byte >> 4) & 0x03) * 0.15;
      if (jagged_strength > 0) {
        uint8_t pos_hash = (x * 13 + y * 29);
        d += sin(pos_hash * 3.14159 / 7.0) * jagged_strength;
      }

      static_distances[x][y] = d;
    }
  }

  pattern_dirty = false; // Pre-calculation is done
}

void drawSymmetricalPattern(byte uid[], byte uidSize, uint32_t t) {
  // If pattern has changed, pre-calculate its static properties
  if (pattern_dirty) {
    precalculatePattern(uid, uidSize);
  }

  // Clear frame buffer instead of matrix directly
  for (int i = 0; i < 13 * 9; i++) {
    frame_buffer[i] = 0;
  }

  if (uidSize < 7) {
    // Not enough data to generate a pattern
    return;
  }

  // The center pixel color is determined by the pattern_seed_byte
  frame_buffer[6 + 4 * 13] = cached_palette[cached_pattern_seed_byte % 5];

  int centerX = 6;
  int centerY = 4;

  // Iterate over top-left quadrant and mirror
  for (int y = 0; y <= centerY; y++) {
    for (int x = 0; x <= centerX; x++) {
      if (x == centerX && y == centerY)
        continue; // Center pixel already drawn

      // Load pre-calculated distance
      float d = static_distances[x][y];

      if (d < 0)
        d = 0;

      // Apply ring size modifier before animation to keep speed consistent
      d /= cached_ring_size_modifier;

      // --- Animation ---
      // Creates a recursive scaling effect where rings expand outwards, and
      // the outer rings "wrap around" to become the new inner rings. This
      // gives the illusion of an infinitely zooming or unfolding pattern.
      float animation_speed = 0.001; // Controls how fast the pattern animates
      float animation_offset = t * animation_speed;
      float ring_cycle_period = 4.0; // The pattern repeats every 4 rings

      // Subtracting the offset makes rings expand. The positive modulo
      // operation creates the seamless wrapping (zooming) effect.
      float d_shifted = d - animation_offset;
      d = fmod(fmod(d_shifted, ring_cycle_period) + ring_cycle_period,
               ring_cycle_period);

      // Determine which ring the pixel belongs to
      int ring_index = floor(d);

      if (ring_index < 1)
        continue; // Skip pixels too close to the center

      // --- 3. Sparsity and Pixel Activation ---
      // Hash pixel coordinates and UID bytes to create a deterministic check
      uint32_t hash = (cached_pattern_seed_byte * 31 + x) * 31 + y;
      hash = (hash ^ cached_style_byte) * 31 + ring_index;

      // Use a threshold based on sparsity_byte.
      // A lower sparsity_byte results in a denser pattern.
      if ((hash % 256) > cached_sparsity_byte) {
        // Use pattern_seed_byte and ring_index to select color
        uint32_t color =
            cached_palette[(cached_pattern_seed_byte + ring_index) % 5];

        // Draw the pixel in all 4 quadrants for symmetry
        frame_buffer[x + y * 13] = color;              // Top-left
        frame_buffer[(12 - x) + y * 13] = color;       // Top-right
        frame_buffer[x + (8 - y) * 13] = color;        // Bottom-left
        frame_buffer[(12 - x) + (8 - y) * 13] = color; // Bottom-right
      }
    }
  }
}

// This function will be called repeatedly in loop() to draw animation frames
void updateAnimation() {
  uint32_t t = millis();
  drawSymmetricalPattern(current_uid, current_uid_size, t);

  // Copy frame buffer to matrix
  for (int y = 0; y < 9; y++) {
    for (int x = 0; x < 13; x++) {
      matrix.drawPixel(x, y, frame_buffer[x + y * 13]);
    }
  }

  frame_ready = true;
}

// Function to display error pattern
void displayErrorPattern() {
  matrix.fill(0);

  // Display a red X pattern for errors
  uint32_t red = matrix.Color(255, 0, 0);

  // Draw X pattern
  for (int16_t i = 0; i < min(13, 9); i++) {
    if (i < 13 && i < 9) {
      matrix.drawPixel(i, i, red);
      matrix.drawPixel(12 - i, i, red);
    }
  }

  matrix.show();
  // Serial.println("Error pattern displayed");
}

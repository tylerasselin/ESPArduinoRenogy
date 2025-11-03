#include <ModbusMaster.h>

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ModbusMaster.h>
#include <ArduinoOTA.h>
#include <Arduino_JSON.h>
#include <WiFiManager.h>  // WiFiManager library for easy WiFi configuration

String wifiHostname = "RenogyESP";

ESP8266WebServer server(80);
WiFiManager wifiManager;

ModbusMaster node;

/*
Number of registers to check. I think all Renogy controllers have 35
data registers (not all of which are used) and 17 info registers.
*/
const uint32_t num_data_registers = 35;
const uint32_t num_info_registers = 17;

// if you don't have a charge controller to test with, can set this to true to get non 0 voltage readings
bool simulator_mode = false;


// A struct to hold the controller data
struct Controller_data {

  uint8_t battery_soc;                // percent
  float battery_voltage;              // volts
  float battery_charging_amps;        // amps
  uint8_t battery_temperature;        // celcius
  uint8_t controller_temperature;     // celcius
  float load_voltage;                 // volts
  float load_amps;                    // amps
  bool load_status;                   // bool
  uint8_t load_watts;                 // watts
  float solar_panel_voltage;          // volts
  float solar_panel_amps;             // amps
  uint8_t solar_panel_watts;          // watts
  float min_battery_voltage_today;    // volts
  float max_battery_voltage_today;    // volts
  float max_charging_amps_today;      // amps
  float max_discharging_amps_today;   // amps
  uint8_t max_charge_watts_today;     // watts
  uint8_t max_discharge_watts_today;  // watts
  uint8_t charge_amphours_today;      // amp hours
  uint8_t discharge_amphours_today;   // amp hours
  uint8_t charge_watthours_today;     // watt hours
  uint8_t discharge_watthours_today;  // watt hours
  uint8_t controller_uptime_days;     // days
  uint8_t total_battery_overcharges;  // count
  uint8_t total_battery_fullcharges;  // count

  // Register 0x0120 - Load Status, Load Brightness, Charging State
  uint8_t charging_state;             // 0-6: 0=deactivated, 1=activated, 2=mppt, 3=equalize, 4=boost, 5=float, 6=limiting
  uint8_t load_brightness;            // 0-100 percent (from street light controller)
  bool street_light_status;           // bool (from street light controller)
  uint16_t charging_state_raw;        // raw register 0x0120 value for debugging

  // WiFi signal monitoring
  int8_t wifi_rssi;                   // WiFi signal strength in dBm (e.g., -65)
  const char* wifi_quality;           // Human-readable quality: "Excellent", "Good", "Fair", "Weak", "Poor"

  // convenience values
  float battery_temperatureF;     // fahrenheit
  float controller_temperatureF;  // fahrenheit
  float battery_charging_watts;   // watts. necessary? Does it ever differ from solar_panel_watts?
  long last_update_time;          // millis() of last update time
  bool controller_connected;      // bool if we successfully read data from the controller
};
Controller_data renogy_data;


// A struct to hold the controller info params
struct Controller_info {

  uint8_t voltage_rating;        // volts
  uint8_t amp_rating;            // amps
  uint8_t discharge_amp_rating;  // amps
  uint8_t type;
  uint8_t controller_name;
  char software_version[40];
  char hardware_version[40];
  char serial_number[40];
  uint8_t modbus_address;
  float wattage_rating;
  long last_update_time;  // millis() of last update time
};
Controller_info renogy_info;

// Helper function to convert charging state value to human-readable string
const char* get_charging_state_string(uint8_t state) {
  switch(state) {
    case 0: return "Deactivated";
    case 1: return "Activated";
    case 2: return "MPPT/Bulk";
    case 3: return "Equalizing";
    case 4: return "Boost";
    case 5: return "Floating";
    case 6: return "Current Limiting";
    default: return "Unknown";
  }
}

// Helper function to convert WiFi RSSI (dBm) to human-readable quality string
const char* get_wifi_quality_string(int8_t rssi) {
  if (rssi >= -50) return "Excellent";
  else if (rssi >= -60) return "Good";
  else if (rssi >= -70) return "Fair";
  else if (rssi >= -80) return "Weak";
  else return "Poor";
}

// Poll the data from the controller and store it in the renogy_info and renogy_data structs
void readRenogyRegisters(){
  static uint32_t i;
  i++;

  // set word 0 of TX buffer to least-significant word of counter (bits 15..0)
  node.setTransmitBuffer(0, lowWord(i));
  // set word 1 of TX buffer to most-significant word of counter (bits 31..16)
  node.setTransmitBuffer(1, highWord(i));

  // Read WiFi signal strength
  renogy_data.wifi_rssi = WiFi.RSSI();
  renogy_data.wifi_quality = get_wifi_quality_string(renogy_data.wifi_rssi);

  renogy_read_data_registers();
  renogy_read_info_registers();
}


// Generates the HTML homepage showing the stats from the controller
void handleRoot() {
  readRenogyRegisters();
  String message = "<html><body><table><tr><th colspan='2'>Renogy Wanderer Stats</th></tr>";

  // WiFi Signal - prominently displayed at top
  message += "<tr><td><strong>WiFi Signal</strong></td><td><strong>" + String(renogy_data.wifi_rssi) + " dBm (" + String(renogy_data.wifi_quality) + ")</strong></td>";

  message += "<tr><td>Battery State of Charge</td><td>" + String(renogy_data.battery_soc) + " %</td>";
  message += "<tr><td>Battery Voltage</td><td>" + String(renogy_data.battery_voltage) + " V</td>";
  message += "<tr><td>Battery Charge Current</td><td>" + String(renogy_data.battery_charging_amps) + " A</td>";
  message += "<tr><td>Battery Charge Power</td><td>" + String(renogy_data.battery_charging_watts) + " W</td>";

  // Charging State - prominently displayed with debug info
  message += "<tr><td><strong>Charging Mode</strong></td><td><strong>" + String(get_charging_state_string(renogy_data.charging_state)) + "</strong> (0x" + String(renogy_data.charging_state_raw, HEX) + ")</td>";

  message += "<tr><td>Controller Temperature</td><td>" + String(renogy_data.controller_temperature) + "&deg;C</td>";
  message += "<tr><td>Battery Temperature</td><td>" + String(renogy_data.battery_temperature) + "&deg;C</td>";
  message += "<tr><td>Load Voltage</td><td>" + String(renogy_data.load_voltage) + " V</td>";
  message += "<tr><td>Load Current</td><td>" + String(renogy_data.load_amps) + " A</td>";
  message += "<tr><td>Load Power</td><td>" + String(renogy_data.load_watts) + " W</td>";
  message += "<tr><td>Solar Panel Voltage</td><td>" + String(renogy_data.solar_panel_voltage) + " V</td>";
  message += "<tr><td>Solar Panel Current</td><td>" + String(renogy_data.solar_panel_amps) + " A</td>";
  message += "<tr><td>Solar Panel Power</td><td>" + String(renogy_data.solar_panel_watts) + " W</td>";
  message += "<tr><td>Minimum Battery Voltage Today</td><td>" + String(renogy_data.min_battery_voltage_today) + " V</td>";
  message += "<tr><td>Maximum Battery Voltage Today</td><td>" + String(renogy_data.max_battery_voltage_today) + " V</td>";
  message += "<tr><td>Maximum Charge Current Today</td><td>" + String(renogy_data.max_charging_amps_today) + " A</td>";
  message += "<tr><td>Maximum Discharging Current Today</td><td>" + String(renogy_data.max_discharging_amps_today) + " A</td>";
  message += "<tr><td>Maximum Charge Power Today</td><td>" + String(renogy_data.max_charge_watts_today) + " W</td>";
  message += "<tr><td>Maximum Discharge Power Today</td><td>" + String(renogy_data.max_discharge_watts_today) + " W</td>";
  message += "<tr><td>Charge Ampere-Hours Today</td><td>" + String(renogy_data.charge_amphours_today) + " Ah</td>";
  message += "<tr><td>Discharge Ampere-Hours Today</td><td>" + String(renogy_data.discharge_amphours_today) + " Ah</td>";
  message += "<tr><td>Charge Watt-Hours Today</td><td>" + String(renogy_data.charge_watthours_today) + " Wh</td>";
  message += "<tr><td>Discharge Watt-Hours Today</td><td>" + String(renogy_data.discharge_watthours_today) + " Wh</td>";
  message += "<tr><td>Controller Uptime (Days)</td><td>" + String(renogy_data.controller_uptime_days) + " D</td>";
  message += "<tr><td>Total Battery Overcharges</td><td>" + String(renogy_data.total_battery_overcharges) + "</td>";
  message += "<tr><td>Total Battery Full Charges</td><td>" + String(renogy_data.total_battery_fullcharges) + "</td>";
  message += "<tr><td>Last Update Time</td><td>" + String(renogy_data.last_update_time) + "</td>";

  message += "<tr><td>Voltage Rating</td><td>" + String(renogy_info.voltage_rating) + " V</td>";
  message += "<tr><td>Amp Rating</td><td>" + String(renogy_info.amp_rating) + " A</td>";
  message += "<tr><td>Discharge Amp Rating</td><td>" + String(renogy_info.discharge_amp_rating) + " A</td>";
  message += "<tr><td>Type</td><td>" + String(renogy_info.type) + "</td>";
  message += "<tr><td>Controller Name</td><td>" + String(renogy_info.controller_name) + "</td>";
  message += "<tr><td>Software Version</td><td>" + String(renogy_info.software_version) + "</td>";
  message += "<tr><td>Hardware Version</td><td>" + String(renogy_info.hardware_version) + "</td>";
  message += "<tr><td>Serial Number</td><td>" + String(renogy_info.serial_number) + "</td>";

  message += "<tr><th colspan='2'>Load Control</th></tr>";

  if(renogy_data.load_status){
    message += "<tr><td>Load Status</td><td>On</td>";
  } else {
    message += "<tr><td>Load Status</td><td>Off</td>";
  }
  message += "<tr><td><a href='/load?on'>On</a></td><td><a href='/load?off'>Off</a></td>";

  message += "<tr><th colspan='2'>Configuration</th></tr>";
  message += "<tr><td colspan='2'><a href='/wificonfig'>Configure WiFi Settings</a></td></tr>";

  message += "</table></body></html>";

  server.send(200, "text/html", message);
}

// Generates the JSON/REST output at the /rest URL
void restView() {
  readRenogyRegisters();
  JSONVar jsonDoc;

  jsonDoc["battery_soc"] = renogy_data.battery_soc;
  jsonDoc["battery_voltage"] = renogy_data.battery_voltage;
  jsonDoc["battery_charging_amps"] = renogy_data.battery_charging_amps;
  jsonDoc["battery_charging_watts"] = renogy_data.battery_charging_watts;
  jsonDoc["controller_temperature"] = renogy_data.controller_temperature;
  jsonDoc["battery_temperature"] = renogy_data.battery_temperature;
  jsonDoc["controller_temperatureF"] = renogy_data.controller_temperatureF;
  jsonDoc["battery_temperatureF"] = renogy_data.battery_temperatureF;
  jsonDoc["load_voltage"] = renogy_data.load_voltage;
  jsonDoc["load_amps"] = renogy_data.load_amps;
  jsonDoc["load_watts"] = renogy_data.load_watts;
  jsonDoc["load_status"] = renogy_data.load_status;
  jsonDoc["solar_panel_voltage"] = renogy_data.solar_panel_voltage;
  jsonDoc["solar_panel_amps"] = renogy_data.solar_panel_amps;
  jsonDoc["solar_panel_watts"] = renogy_data.solar_panel_watts;
  jsonDoc["min_battery_voltage_today"] = renogy_data.min_battery_voltage_today;
  jsonDoc["max_battery_voltage_today"] = renogy_data.max_battery_voltage_today;
  jsonDoc["max_charging_amps_today"] = renogy_data.max_charging_amps_today;
  jsonDoc["max_discharging_amps_today"] = renogy_data.max_discharging_amps_today;
  jsonDoc["max_charge_watts_today"] = renogy_data.max_charge_watts_today;
  jsonDoc["max_discharge_watts_today"] = renogy_data.max_discharge_watts_today;
  jsonDoc["charge_amphours_today"] = renogy_data.charge_amphours_today;
  jsonDoc["discharge_amphours_today"] = renogy_data.discharge_amphours_today;
  jsonDoc["charge_watthours_today"] = renogy_data.charge_watthours_today;
  jsonDoc["discharge_watthours_today"] = renogy_data.discharge_watthours_today;
  jsonDoc["controller_uptime_days"] = renogy_data.controller_uptime_days;
  jsonDoc["total_battery_overcharges"] = renogy_data.total_battery_overcharges;
  jsonDoc["total_battery_fullcharges"] = renogy_data.total_battery_fullcharges;
  jsonDoc["last_update_time"] = renogy_data.last_update_time;

  // WiFi signal information
  jsonDoc["wifi_rssi"] = renogy_data.wifi_rssi;
  jsonDoc["wifi_quality"] = renogy_data.wifi_quality;

  // Charging state information (register 0x0120)
  jsonDoc["charging_state"] = renogy_data.charging_state;
  jsonDoc["charging_state_label"] = get_charging_state_string(renogy_data.charging_state);
  jsonDoc["charging_state_raw"] = renogy_data.charging_state_raw;
  jsonDoc["street_light_status"] = renogy_data.street_light_status;
  jsonDoc["load_brightness"] = renogy_data.load_brightness;

  jsonDoc["voltage_rating"] = renogy_info.voltage_rating;
  jsonDoc["amp_rating"] = renogy_info.amp_rating;
  jsonDoc["discharge_amp_rating"] = renogy_info.discharge_amp_rating;
  jsonDoc["type"] = renogy_info.type;
  jsonDoc["controller_name"] = renogy_info.controller_name;
  jsonDoc["software_version"] = renogy_info.software_version;
  jsonDoc["hardware_version"] = renogy_info.hardware_version;
  jsonDoc["serial_number"] = renogy_info.serial_number;

  server.send(200, "application/json", JSON.stringify(jsonDoc));
}

// Allow control of the load
void toggleLoad(){
  if(server.hasArg("on")){
    renogy_control_load(1);
  }
  else if(server.hasArg("off")){
    renogy_control_load(0);
  }
  // It takes a moment for the command to take effect
  delay(1000);
  // Redirect to homepage
  server.sendHeader("Location", "/", true);
  server.send(302);
}

// Start WiFi configuration portal on-demand
void handleWifiConfig(){
  String message = "<html><body>";
  message += "<h2>WiFi Configuration</h2>";
  message += "<p>Starting WiFi configuration portal...</p>";
  message += "<p>1. Connect to the WiFi network: <strong>RenogyESP-Config</strong></p>";
  message += "<p>2. A configuration page should open automatically</p>";
  message += "<p>3. If not, navigate to <strong>192.168.4.1</strong></p>";
  message += "<p>4. Enter your new WiFi credentials and save</p>";
  message += "<p>5. The device will restart and connect to the new network</p>";
  message += "<p><a href='/'>Return to home</a></p>";
  message += "</body></html>";
  server.send(200, "text/html", message);

  // Start the configuration portal (non-blocking for 180 seconds)
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.startConfigPortal("RenogyESP-Config");
}

// List the modbus registers for debugging
void modbustest(){
  static uint32_t i;
  i++;

  // set word 0 of TX buffer to least-significant word of counter (bits 15..0)
  node.setTransmitBuffer(0, lowWord(i));
  // set word 1 of TX buffer to most-significant word of counter (bits 31..16)
  node.setTransmitBuffer(1, highWord(i));

  uint8_t j, result1;
  uint16_t data_registers[num_data_registers];
  String message = "Modbus Data: \n";

  result1 = node.readHoldingRegisters(0x100, num_data_registers);
  if (result1 == node.ku8MBSuccess) {
    message += "Successfully read the data registers!\n";
    for (j = 0; j < num_data_registers; j++) {
      data_registers[j] = node.getResponseBuffer(j);

      message += String(j) + ": " + String(data_registers[j]) + "\n";
    }

    // Detailed breakdown of register 32 (0x0120) - Charging State
    message += "\n=== Register 32 (0x0120) Detailed Breakdown ===\n";
    uint16_t reg_0x0120 = data_registers[32];
    message += "Raw Value (Hex): 0x" + String(reg_0x0120, HEX) + "\n";
    message += "Raw Value (Dec): " + String(reg_0x0120) + "\n";
    message += "Raw Value (Bin): ";
    for (int bit = 15; bit >= 0; bit--) {
      message += ((reg_0x0120 >> bit) & 1) ? "1" : "0";
      if (bit == 8) message += " ";  // Space between bytes
    }
    message += "\n\n";

    // Parse components
    uint8_t charging_state = reg_0x0120 & 0x00FF;
    bool street_light = (reg_0x0120 >> 15) & 0x01;
    uint8_t brightness = (reg_0x0120 >> 8) & 0x7F;

    message += "High Byte (0x" + String((reg_0x0120 >> 8), HEX) + "):\n";
    message += "  - Bit 15 (Street Light Status): " + String(street_light) + (street_light ? " (ON)" : " (OFF)") + "\n";
    message += "  - Bits 8-14 (Load Brightness): " + String(brightness) + "%\n";
    message += "\n";
    message += "Low Byte (0x" + String(charging_state, HEX) + "):\n";
    message += "  - Charging State Value: " + String(charging_state) + "\n";
    message += "  - Charging State: " + String(get_charging_state_string(charging_state)) + "\n";
    message += "\nCharging State Legend:\n";
    message += "  0 = Deactivated\n";
    message += "  1 = Activated\n";
    message += "  2 = MPPT/Bulk\n";
    message += "  3 = Equalizing\n";
    message += "  4 = Boost\n";
    message += "  5 = Floating\n";
    message += "  6 = Current Limiting\n";
    message += "==========================================\n\n";
  }

  uint8_t k, result2;
  uint16_t info_registers[num_info_registers];

  result2 = node.readHoldingRegisters(0x00A, num_info_registers);
  if (result2 == node.ku8MBSuccess) {
    message += "Successfully read the info registers!\n";
    for (k = 0; k < num_info_registers; k++) {
      info_registers[k] = node.getResponseBuffer(k);
      message += String(k) + ": " + String(data_registers[k]) + "\n";
    }
  }

  server.send(200, "text/plain", message);
}

// Gracefully handle invalid URLs
void handleNotFound() {

  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) { message += " " + server.argName(i) + ": " + server.arg(i) + "\n"; }
  server.send(404, "text/plain", message);
}

void setup(void) {

  // ESP8266 only has one hardware serial UART, which I'm using with the Wanderer
  // If you're using an ESP32 or other MCU with more than one UART you will
  // probably want to use a second serial port for this and use the "main" one for debugging
  // I have commented out any serial debugging that was in the original code

  Serial.begin(9600, SERIAL_8N1);
  // Serial.begin(9600); // For debugging

  Serial.println(F("Starting..."));

  int modbus_address = 255;
  node.begin(modbus_address, Serial);

  // WiFiManager auto-connects to saved WiFi credentials
  // If connection fails or no credentials saved, it starts a config portal
  WiFi.mode(WIFI_STA);
  WiFi.hostname(wifiHostname);

  Serial.print("\r\nMAC Address: ");
  Serial.println(WiFi.macAddress());

  // Auto-connect to saved WiFi or start config portal if needed
  // Will create AP "RenogyESP-AutoConnect" if no saved credentials
  wifiManager.setConfigPortalTimeout(180);  // 3 minute timeout for config portal
  if (!wifiManager.autoConnect("RenogyESP-AutoConnect")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    // Reset and try again
    ESP.restart();
    delay(5000);
  }

  Serial.print("\r\nConnected: local ip address is http://");
  Serial.println(WiFi.localIP());

  MDNS.begin(wifiHostname);

  // Webserver Pages
  server.on("/", handleRoot);
  server.on("/rest", restView);
  server.on("/load", toggleLoad);
  server.on("/wificonfig", handleWifiConfig);
  server.on("/modbustest", modbustest);
  server.onNotFound(handleNotFound);

  server.begin();
  ArduinoOTA.begin();
}

void loop(void) {
  server.handleClient();
  MDNS.update();
  ArduinoOTA.handle();
}


void renogy_read_data_registers() {
  uint8_t j, result;
  uint16_t data_registers[num_data_registers];
  char buffer1[40], buffer2[40];
  uint8_t raw_data;

  // prints data about each read to the console
  bool print_data = 0;

  result = node.readHoldingRegisters(0x100, num_data_registers);
  if (result == node.ku8MBSuccess) {
    if (print_data) {
      Serial.println("Successfully read the data registers!");
    }
    renogy_data.controller_connected = true;
    for (j = 0; j < num_data_registers; j++) {
      data_registers[j] = node.getResponseBuffer(j);
      if (print_data) {
        Serial.println(data_registers[j]);
      }
    }

    renogy_data.battery_soc = data_registers[0];
    renogy_data.battery_voltage = data_registers[1] * .1;  // will it crash if data_registers[1] doesn't exist?
    renogy_data.battery_charging_amps = data_registers[2] * .01;

    renogy_data.battery_charging_watts = renogy_data.battery_voltage * renogy_data.battery_charging_amps;

    //0x103 returns two bytes, one for battery and one for controller temp in c
    uint16_t raw_data = data_registers[3];  // eg 5913
    renogy_data.controller_temperature = raw_data / 256;
    renogy_data.battery_temperature = raw_data % 256;
    // for convenience, fahrenheit versions of the temperatures
    renogy_data.controller_temperatureF = (renogy_data.controller_temperature * 1.8) + 32;
    renogy_data.battery_temperatureF = (renogy_data.battery_temperature * 1.8) + 32;

    renogy_data.load_voltage = data_registers[4] * .1;
    renogy_data.load_amps = data_registers[5] * .01;
    renogy_data.load_watts = data_registers[6];

    // The load is on if the load voltage is above zero
    if(renogy_data.load_voltage > 0){
      renogy_data.load_status = 1;
    } else {
      renogy_data.load_status = 0;
    }

    renogy_data.solar_panel_voltage = data_registers[7] * .1;
    renogy_data.solar_panel_amps = data_registers[8] * .01;
    renogy_data.solar_panel_watts = data_registers[9];

    //Register 0x10A - Turn on load, write register, unsupported in wanderer - 10
    renogy_data.min_battery_voltage_today = data_registers[11] * .1;
    renogy_data.max_battery_voltage_today = data_registers[12] * .1;
    renogy_data.max_charging_amps_today = data_registers[13] * .01;
    renogy_data.max_discharging_amps_today = data_registers[14] * .1;
    renogy_data.max_charge_watts_today = data_registers[15];
    renogy_data.max_discharge_watts_today = data_registers[16];
    renogy_data.charge_amphours_today = data_registers[17];
    renogy_data.discharge_amphours_today = data_registers[18];
    renogy_data.charge_watthours_today = data_registers[19];
    renogy_data.discharge_watthours_today = data_registers[20];
    renogy_data.controller_uptime_days = data_registers[21];
    renogy_data.total_battery_overcharges = data_registers[22];
    renogy_data.total_battery_fullcharges = data_registers[23];

    // Register 0x120 - Load Status, Load Brightness, Charging State - index 32
    // High byte: bit 7 = street light status, bits 0-6 = load brightness (0-100%)
    // Low byte: charging state (0-6)
    renogy_data.charging_state_raw = data_registers[32];
    renogy_data.charging_state = renogy_data.charging_state_raw & 0x00FF;  // Extract low byte
    renogy_data.street_light_status = (renogy_data.charging_state_raw >> 15) & 0x01;  // Extract bit 15
    renogy_data.load_brightness = (renogy_data.charging_state_raw >> 8) & 0x7F;  // Extract bits 8-14

    renogy_data.last_update_time = millis();

    // TODO: Add these registers in future:
    //Registers 0x118 to 0x119- Total Charging Amp-Hours - 24/25
    //Registers 0x11A to 0x11B- Total Discharging Amp-Hours - 26/27
    //Registers 0x11C to 0x11D- Total Cumulative power generation (kWH) - 28/29
    //Registers 0x11E to 0x11F- Total Cumulative power consumption (kWH) - 30/31
    //Registers 0x121 to 0x122 - Controller fault codes - 33/34

    if (print_data) Serial.println("---");
  } else {
    if (result == 0xE2) {
       //Serial.println("Timed out reading the data registers!");
    } else {
       //Serial.print("Failed to read the data registers... ");
       //Serial.println(result, HEX);  // E2 is timeout
    }
    // Reset some values if we don't get a reading
    renogy_data.controller_connected = false;
    renogy_data.battery_voltage = 0;
    renogy_data.battery_charging_amps = 0;
    renogy_data.battery_soc = 0;
    renogy_data.battery_charging_amps = 0;
    renogy_data.controller_temperature = 0;
    renogy_data.battery_temperature = 0;
    renogy_data.solar_panel_amps = 0;
    renogy_data.solar_panel_watts = 0;
    renogy_data.battery_charging_watts = 0;
    if (simulator_mode) {
      renogy_data.battery_voltage = 13.99;
      renogy_data.battery_soc = 55;
    }
  }
}


void renogy_read_info_registers() {
  uint8_t j, result;
  uint16_t info_registers[num_info_registers];
  char buffer1[40], buffer2[40];
  uint8_t raw_data;

  // prints data about the read to the console
  bool print_data = 0;

  result = node.readHoldingRegisters(0x00A, num_info_registers);
  if (result == node.ku8MBSuccess) {
    if (print_data) Serial.println("Successfully read the info registers!");
    for (j = 0; j < num_info_registers; j++) {
      info_registers[j] = node.getResponseBuffer(j);
      if (print_data) Serial.println(info_registers[j]);
    }

    // read and process each value
    //Register 0x0A - Controller voltage and Current Rating - 0
    // Not sure if this is correct. I get the correct amp rating for my Wanderer 30 (30 amps), but I get a voltage rating of 0 (should be 12v)
    raw_data = info_registers[0];
    renogy_info.voltage_rating = raw_data / 256;
    renogy_info.amp_rating = raw_data % 256;
    renogy_info.wattage_rating = renogy_info.voltage_rating * renogy_info.amp_rating;
    //Serial.println("raw ratings = " + String(raw_data));
    //Serial.println("Voltage rating: " + String(renogy_info.voltage_rating));
    //Serial.println("amp rating: " + String(renogy_info.amp_rating));


    //Register 0x0B - Controller discharge current and type - 1
    raw_data = info_registers[1];
    renogy_info.discharge_amp_rating = raw_data / 256;  // not sure if this should be /256 or /100
    renogy_info.type = raw_data % 256;                  // not sure if this should be /256 or /100

    //Registers 0x0C to 0x13 - Product Model String - 2-9
    // Here's how the nodeJS project handled this:
    /*
    let modelString = '';
    for (let i = 0; i <= 7; i++) {  
        rawData[i+2].toString(16).match(/.{1,2}/g).forEach( x => {
            modelString += String.fromCharCode(parseInt(x, 16));
        });
    }
    this.controllerModel = modelString.replace(' ','');
    */

    //Registers 0x014 to 0x015 - Software Version - 10-11
    itoa(info_registers[10], buffer1, 10);
    itoa(info_registers[11], buffer2, 10);
    strcat(buffer1, buffer2);  // should put a divider between the two strings?
    strcpy(renogy_info.software_version, buffer1);
    //Serial.println("Software version: " + String(renogy_info.software_version));

    //Registers 0x016 to 0x017 - Hardware Version - 12-13
    itoa(info_registers[12], buffer1, 10);
    itoa(info_registers[13], buffer2, 10);
    strcat(buffer1, buffer2);  // should put a divider between the two strings?
    strcpy(renogy_info.hardware_version, buffer1);
    //Serial.println("Hardware version: " + String(renogy_info.hardware_version));

    //Registers 0x018 to 0x019 - Product Serial Number - 14-15
    // I don't think this is correct... Doesn't match serial number printed on my controller
    itoa(info_registers[14], buffer1, 10);
    itoa(info_registers[15], buffer2, 10);
    strcat(buffer1, buffer2);  // should put a divider between the two strings?
    strcpy(renogy_info.serial_number, buffer1);
    //Serial.println("Serial number: " + String(renogy_info.serial_number)); // (I don't think this is correct)

    renogy_info.modbus_address = info_registers[16];

    renogy_info.last_update_time = millis();

    if (print_data) Serial.println("---");
  } else {
    if (result == 0xE2) {
      //Serial.println("Timed out reading the info registers!");
    } else {
       //Serial.print("Failed to read the info registers... ");
       //Serial.println(result, HEX);  // E2 is timeout
    }
    // anything else to do if we fail to read the info reisters?
  }
}


// control the load pins on Renogy charge controllers that have them
void renogy_control_load(bool state) {
  if (state == 1) node.writeSingleRegister(0x010A, 1);  // turn on load
  else node.writeSingleRegister(0x010A, 0);             // turn off load
}

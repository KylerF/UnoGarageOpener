// Garage door opener sketch for Arduino Uno with WiFi sheild.
// Hosts a simple web server which accepts the following requests:
//    /refresh - forces a refresh of the physical door status
//    /trigger - triggers the door button and returns the door status
// Visiting the host URL without any commands will display the status 
// without any other action.
//
// Update 11/24/2018: Send request to home automation server API to 
// log each time the garage is opened or closed, and whether it was
// triggered remotely.
//
// Author: Kyler Freas
// Last updated: 11/24/2018

#include <SPI.h>
#include <WiFi.h>
#include "wifi-auth.h"
#include <ArduinoHttpClient.h>
#include <string.h>

////////////////////// Commands //////////////////////
// Unique ID for each command
enum command_id {
  refresh_command,
  trigger_command,
  invalid_command
};

// Command table lookup entry
struct command_table_entry {
  char *command;      // command string
  enum command_id id; // command ID
};

// Lookup table to map command strings to command IDs
static struct command_table_entry command_table[] = 
{ 
  { "refresh", refresh_command },
  { "trigger", trigger_command },
  { NULL, invalid_command }
};
//////////////////////////////////////////////////////

/////////////////////// Status ///////////////////////
// Unique ID for each door status
enum door_status_id {
  open_status,
  opening_status,
  closed_status,
  closing_status,
  stuck_status,
  cancelled_status,
  error_status
};

// Door status lookup table entry containing all status
// details and transition instructions
struct door_status_table_entry {
  enum door_status_id id;      // status ID
  int  open_switch_value;      // value of the door open switch in this state (0 or 1)
  int  closed_switch_value;    // value of the door closed switch in this state (0 or 1)
  enum door_status_id next_id; // ID of next status to go to after door is triggered
  char * description;          // string description to return
};

// Lookup table for door status
static struct door_status_table_entry door_status_table[] = 
{ 
  { open_status,      1, 0, closing_status,   "open"       },
  { closed_status,    0, 1, opening_status,   "closed"     },
  { error_status,     1, 1, error_status,     "reed_error" },
  { stuck_status,     0, 0, stuck_status,     "stuck"      },
  { opening_status,   0, 0, cancelled_status, "opening"    },
  { cancelled_status, 0, 0, closing_status,   "cancelled"  },
  { closing_status,   0, 0, opening_status,   "closing"    },
  { NULL }
};

// Flag for current door status
int door_status;
/////////////////////////////////////////////////////

/////////////////////// Pins /////////////////////////
const int trigger_door_pin =  4;
const int switch_closed_pin = 3;
const int switch_open_pin  =  2;          
//////////////////////////////////////////////////////

/////////////////////// WiFi /////////////////////////
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

int wifi_status = WL_IDLE_STATUS;

// Local web server to handle HTTP requests
WiFiServer local_server(80);
WiFiClient client;

// Home automation API for logging to MySQL
const char freas_server[] = API_IP;
const int homeautomation_port = API_PORT;
WiFiClient api_client;
HttpClient http_client = HttpClient(api_client, freas_server, homeautomation_port);
//////////////////////////////////////////////////////

// Timeout to wait for door to move (millis)
const unsigned long door_move_timeout = 20000;

// Last time the door was triggered (millis)
unsigned long last_door_trigger;

void setup() {
  Serial.begin(9600); 

  // Digital pin setup
  pinMode(trigger_door_pin, OUTPUT);
  pinMode(switch_open_pin, INPUT);
  pinMode(switch_closed_pin, INPUT);

  // Reed switch interrupts
  attachInterrupt(digitalPinToInterrupt(switch_open_pin), switch_interrupt, RISING);
  attachInterrupt(digitalPinToInterrupt(switch_closed_pin), switch_interrupt, RISING);

  connect_wifi();
  door_status = get_door_status();
  //log_door_status();
}

// Wait for client connection and parse any incoming requests
void loop() {
  client = local_server.available();

  if (client) {
    Serial.println("new client");
    
    while (client.connected()) {
      if (client.available()) {
        // Read incoming commands and parse
        char c = client.read();
        proccess_command(c);
      }
    }
    
    Serial.println("client disconnected");
  }

  // If the timeout has elapsed, refresh the door status
  if (last_door_trigger > 0 && millis() - last_door_trigger >= door_move_timeout)
    door_status = get_door_status();
}

// Attempts to connect to wifi. If there is an issue with the
// wifi shield, print error and halt. Attempts to reconnect
// every 5 seconds on initial connection error.
void connect_wifi() {
  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("No WiFi shield detected"); 
    while(true);
  } 

  while ( wifi_status != WL_CONNECTED) { 
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);  
    wifi_status = WiFi.begin(ssid, pass);

    delay(5000);
  }
  
  local_server.begin();
  print_wifi_status();
}

// Prints all wifi connection details to serial
void print_wifi_status() {
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");

  Serial.print("To see this page in action, open a browser to http://");
  Serial.println(ip);
}

// Process incoming characters and process door commands.
void proccess_command(char c) {
  static String current_line = "";
  
  if (c == '\n') {
    if (current_line.length() == 0) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-type:text/html");
      client.println();
  
      // Complete command received. Return the door status.
      client.print(door_status_string());
      client.println();
      client.stop();
      current_line = "";
    } else {
      current_line = "";
    }
  } else if (c != '\r') {
    current_line += c;
  }

  if (current_line.endsWith("GET /trigger")) {
    trigger_door();
  } else if (current_line.endsWith("GET /refresh")) {
    door_status = get_door_status();
  }
}

// Calls the home automation API to log the door status.
// Called each time the door status changes.
void log_door_status() {
  Serial.println("making POST request");
  String content_type = "application/json";
  String post_data = "\"{'device':'arduino','doorStatus':'" + String(door_status_string()) + "','remoteTrigger':'" + (last_door_trigger > 0) + "'}\"";

  //http_client.post(API_PATH, content_type, post_data);
  http_client.beginRequest();
  http_client.post(API_PATH);
  http_client.sendHeader("Content-Type", content_type);
  http_client.sendHeader("Content-Length", post_data.length());
  http_client.sendHeader("Connection", "close");
  http_client.beginBody();
  http_client.print(post_data);
  http_client.endRequest();

  // Read the status code and body of the response
  Serial.print("Status code: ");
  Serial.println(http_client.responseStatusCode());
  Serial.print("Response: ");
  Serial.println(http_client.responseBody());

  http_client.stop();

  Serial.println("HERE");
}

// ISR for reed switch interrupts
// If either switch goes from low to high, poll the door status immediately
// rather than waiting for the timeout period to end.
void switch_interrupt() {
  Serial.println("ISR");
  // Update door status and log it
  door_status = get_door_status();
  log_door_status();
  
  // Reset the trigger timer
  last_door_trigger = 0;
}

// Refresh the current door status based on reed switch values
int get_door_status() {
  // Read the switches
  int door_open   = digitalRead(switch_open_pin);
  int door_closed = digitalRead(switch_closed_pin);

  // If door opening was and still is cancelled, report no change
  if (door_status == cancelled_status && !door_open && !door_closed)
    return cancelled_status;

  // Look up door status based on switch values
  struct door_status_table_entry * p_entry = door_status_table ;
  
  for ( ; p_entry->description != NULL ; p_entry++ )
    if ((door_open == p_entry->open_switch_value) && (door_closed == p_entry->closed_switch_value)) {
      // Log the status to MySQL and return
      return p_entry->id;
    }
}

// Retrieve the description string from the status lookup table
char * door_status_string() {
  struct door_status_table_entry *p_entry = door_status_table ;
  
  for ( ; p_entry->description != NULL ; p_entry++ )
    if ( door_status == p_entry->id )
      return p_entry->description;
}

// Retrieve the new door status after triggering the door
int next_door_status() {
  struct door_status_table_entry * p_entry = door_status_table ;
  
  for ( ; p_entry->description != NULL ; p_entry++ )
    if ( door_status == p_entry->id )
      return p_entry->next_id;
}

// Triggers the door button and sets the new status manually
void trigger_door() {
  // Do nothing if the door is stuck or a reed switch is faulty
  if (door_status == stuck_status || door_status == error_status) return;

  // Trigger the door button
  digitalWrite(trigger_door_pin, HIGH);
  delay(1000);
  digitalWrite(trigger_door_pin, LOW);

  // Set the status check timer and new door status
  last_door_trigger = millis();
  door_status = next_door_status();
}

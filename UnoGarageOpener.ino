// Garage door opener sketch for Arduino Uno with WiFi sheild.
// Hosts a simple web server which accepts the following requests:
//    /refresh - forces a refresh of the physical door status
//    /trigger - triggers the door button and returns the door status
// Visiting the host URL without any commands will display the status 
// without any other action.
//
// Author: Kyler Freas
// Last updated: 11/24/2018

#include <SPI.h>
#include <WiFi.h>
#include "wifi-auth.h"
#include <string.h>

////////////////////// Commands //////////////////////
// Unique ID for each command
enum command_id {
  refresh_command,
  open_command,
  close_command,
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
  { "open",    open_command    },
  { "close",   close_command   },
  { NULL,      invalid_command }
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
  enum door_status_id id;            // status ID
  int  open_switch_value;            // value of the door open switch in this state (0 or 1)
  int  closed_switch_value;          // value of the door closed switch in this state (0 or 1)
  enum door_status_id next_id_open;  // ID of next status to go to after an open command is received
  enum door_status_id next_id_close; // ID of next status to go to after a close command is received
  char * description;                // string description to return
};

// Lookup table for door status
static struct door_status_table_entry door_status_table[] = 
{ 
  { open_status,    1, 0, open_status,    closing_status, "open"       },
  { closed_status,  0, 1, opening_status, closed_status,  "closed"     },
  { error_status,   1, 1, error_status,   error_status,   "reed_error" },
  { stuck_status,   0, 0, stuck_status,   stuck_status,   "stuck"      },
  { opening_status, 0, 0, opening_status, opening_status, "opening"    },
  { closing_status, 0, 0, opening_status, closing_status,  "closing"    },
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
char * ssid_list[] = SSID_LIST;
char * ssid = ssid_list[0];
char pass[] = SECRET_PASS;

int wifi_status = WL_IDLE_STATUS;

// Local web server to handle HTTP requests
WiFiServer local_server(80);
WiFiClient client;
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

  static int retries = 0;
  while ( wifi_status != WL_CONNECTED) { 
    ssid = ssid_list[retries % 2];
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);  
    wifi_status = WiFi.begin(ssid, pass);

    delay(5000);
    retries++;
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

  // Check for a completed command
  if (current_line.endsWith("GET /open")) {
    trigger_door(open_command);
  } else if (current_line.endsWith("GET /close")) {
    trigger_door(close_command);
  } else if (current_line.endsWith("GET /refresh")) {
    door_status = get_door_status();
  }
  
  if (c == '\n') {
    if (current_line.length() == 0) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-type:text/html");
      client.println();
  
      // Complete command received. Return the door status.
      client.print(door_status_string());
      delay(100);
      client.stop();
      
      current_line = "";
    } else {
      current_line = "";
    }
  } else if (c != '\r') {
    current_line += c;
  }
}

// ISR for reed switch interrupts
// If either switch goes from low to high, poll the door status immediately
// rather than waiting for the timeout period to end.
void switch_interrupt() {
  Serial.println("ISR");
  door_status = get_door_status();
}

// Refresh the current door status based on reed switch values
int get_door_status() {
  // Reset the trigger timer
  last_door_trigger = 0;
  
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
int next_door_status(int command) {
  struct door_status_table_entry * p_entry = door_status_table ;
  
  for ( ; p_entry->description != NULL ; p_entry++ ) {
    if ( door_status == p_entry->id ) {
      if (command == open_command) {
        return p_entry->next_id_open;
      } else if (command == close_command) {
        return p_entry->next_id_close;
      }
    }  
  }
    
  return door_status;
}

// Triggers the door button and sets the new status
void trigger_door(int command) {
  // Set the status check timer and new door status
  last_door_trigger = millis();
  int current_status = door_status;
  door_status = next_door_status(command);
  
  // Do nothing if the status stays the same
  if (current_status == door_status) return;

  // Trigger the door button
  digitalWrite(trigger_door_pin, HIGH);
  delay(1000);
  digitalWrite(trigger_door_pin, LOW);
}


/*
 Name:		HollowClockEsp32.ino
 Created:	7/25/2024 2:28:42 PM
 Author:	Martin Nohr
*/
#include <EEPROM.h>
#include <freertos.h>
#include <stdio.h>
#include <wifi.h>
#include <ESPmDNS.h>
#include <time.h>

SemaphoreHandle_t MutexRotateHandle;

#define HC_VERSION 3  // change this when the settings structure is changed

// Motor and clock parameters
// 2048 * 90 / 12 / 60 = 256
#define STEPS_PER_MIN 256

//#define SAFETY_MOTION (STEPS_PER_MIN) // use this for the ratchet version
#define SAFETY_MOTION (0)

// hold setting information in EEPROM
struct {
    int nVersion = HC_VERSION;  // this int must be first
    bool bReverse = false;
    bool bTestMode = false;
    int nStepSpeed = 2;
    int nSafetyMotion = 0;
    char cWifiID[20] = "";
    char cWifiPWD[20] = "";
    long utcOffsetInSeconds = -7 * 3600;
	bool bDST = false;
} settings;

// Set web server port number to 80
WiFiServer server(80);
bool bWifiConnected = false;

// ports used to control the stepper motor
// if your motor rotates in the opposite direction, 
// change the order as {2, 3, 4, 5};
static int port[4] = { 10, 11, 12, 13 };

// sequence of stepper motor control
static int seq[4][4] = {
  {  LOW,  LOW, HIGH,  LOW},
  {  LOW,  LOW,  LOW, HIGH},
  { HIGH,  LOW,  LOW,  LOW},
  {  LOW, HIGH,  LOW,  LOW}
};

void rotate(int step)
{
	if (xSemaphoreTake(MutexRotateHandle, portMAX_DELAY) == pdTRUE) {
		// wait for a single step of stepper
		int delaytime = settings.nStepSpeed;    // was 6 originally
		static int phase = 0;
		int i, j;
		int delta = (step < 0 || settings.bReverse) ? 3 : 1;
		int dt = delaytime * 3;

		step = abs(step);
		for (j = 0; j < step; j++) {
			phase = (phase + delta) % 4;
			for (i = 0; i < 4; i++) {
				digitalWrite(port[i], seq[phase][i]);
			}
			delay(dt);
			if (dt > delaytime)
				--dt;
		}
		// power cut
		for (i = 0; i < 4; i++) {
			digitalWrite(port[i], LOW);
		}
		xSemaphoreGive(MutexRotateHandle);
	}
}

TaskHandle_t TaskMenuHandle;
TaskHandle_t TaskClockMinuteHandle;
TaskHandle_t TaskWifiHandle;
TaskHandle_t TaskServerHandle;
volatile bool bGotTime = false;
struct tm gtime;

void TaskMinutes(void* params)
{
	unsigned long secCounter = 0;
    // use this to make task run every second
    TickType_t xLastWakeTime;
	const TickType_t xFrequency = pdMS_TO_TICKS(1000);
    // Initialize the xLastWakeTime variable with the current time.
    xLastWakeTime = xTaskGetTickCount();
    Serial.println("waiting for internet time");
    // wait for WiFi time to be ready
    while (!bGotTime) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    // calculate how far to move the clock from noon
    int howfar = (gtime.tm_hour % 12) * 60 + gtime.tm_min;
    //Serial.println(String("goto time: ") + howfar);
    // if after 6 take the shorter route in reverse
    if (howfar > 6 * 60) {
        howfar -= 12 * 60;
    }
	// since we move 1 minute right away, let's subtract it first
	rotate(-STEPS_PER_MIN);
    // assume starting at noon
    rotate(STEPS_PER_MIN * howfar);
    for (;;) {
		if (!settings.bTestMode && (secCounter++ % 60 == 0)) {
			rotate(STEPS_PER_MIN + SAFETY_MOTION); // go too far to handle ratchet (might not be there if 0)
			if (SAFETY_MOTION)
				rotate(-SAFETY_MOTION); // alignment
		}
		// Wait for the next cycle.
		vTaskDelayUntil(&xLastWakeTime, xFrequency);
	}
}

void TaskMenu(void* params)
{
	Serial.println("? for menu");
	for (;;) {
		if (Serial.available()) {
			String str;
			str = Serial.readString();
			str.trim();
			Serial.println("Received:" + str);
			if (str.isEmpty())
				str = "?";
			bool bSave = true;
			char ch = str[0];
			str = str.substring(1);
			str.trim();
			switch (toupper(ch)) {
			case '?':
				Serial.println("---------------------------");
				Serial.print(String("Last Sync  : "));
				Serial.println(&gtime, "%A, %B %d %Y %H:%M:%S");
				Serial.println(String("Network    : ") + settings.cWifiID);
				Serial.println(String("Password   : ") + settings.cWifiPWD);
				Serial.println(String("UTC        : ") + (settings.utcOffsetInSeconds / 3600));
				Serial.println(String("DST        : ") + (settings.bDST ? "ON" : "OFF"));
				Serial.println(String("Step Delay : ") + settings.nStepSpeed + " mS");
				Serial.println(String("Test mode  : ") + (settings.bTestMode ? "ON" : "OFF"));
				Serial.println("---------------------------");
				Serial.println("N<networkID>  = network name (case sensitive)");
				Serial.println("P<password>   = password for network (case sensitive)");
				Serial.println("U<-12 to +12> = utc offset in hours");
				Serial.println("D             = toggle daylight saving (DST)");
				Serial.println("S<2 to 10>    = stepper delay in mS");
				Serial.println("T             = toggle test mode");
				Serial.println("+<n>          = add one or more minutes");
				Serial.println("-<n>          = subtract one or more minutes");
				Serial.println();
				bSave = false;
				break;
			case 'N':
				if (str.length()) {
					strncpy(settings.cWifiID, str.c_str(), sizeof(settings.cWifiID) - 1);
					Serial.println("Network Name:" + str);
				}
				break;
			case 'P':
				if (str.length()) {
					strncpy(settings.cWifiPWD, str.c_str(), sizeof(settings.cWifiPWD) - 1);
					Serial.println("Password:" + str);
				}
				break;
			case 'T':
				settings.bTestMode = !settings.bTestMode;
				break;
			case 'U':
				settings.utcOffsetInSeconds = str.toInt() * 3600;
				break;
			case 'D':
				settings.bDST = !settings.bDST;
				// adjust the time
				rotate((settings.bDST ? 1 : -1) * 60 * STEPS_PER_MIN);
				break;
			case 'S':
				settings.nStepSpeed = str.toInt();
				break;
			case '+':
				if (str.length()) {
					rotate(str.toInt() * STEPS_PER_MIN);
				}
				else {
					rotate(STEPS_PER_MIN);
				}
				break;
			case '-':
				if (str.length()) {
					rotate(-str.toInt() * STEPS_PER_MIN);
				}
				else {
					rotate(-STEPS_PER_MIN);
				}
				break;
			}
			if (bSave) {
				EEPROM.put(0, settings);
				EEPROM.commit();
			}
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

/*
* get the time once per day and compare and correct if necessary
* TODO: the correction is not there yet
*/
void TaskWiFi(void* params)
{
	TickType_t xLastWakeTime;
	// check the time once per day
	const TickType_t xFrequency = pdMS_TO_TICKS(24 * 60 * 60 * 1000);
	// Initialize the xLastWakeTime variable with the current time.
	xLastWakeTime = xTaskGetTickCount();
	// init the WiFi
    WiFi.mode(WIFI_AP_STA);
	// if no network, try and get setup
	if (settings.cWifiID[0] == '\0') {
		WiFi.beginSmartConfig();
		//Wait for SmartConfig packet from mobile
		Serial.println("Waiting for SmartConfig.");
		int waitForSmartConfig = 2 * 60;
		while (waitForSmartConfig-- && !WiFi.smartConfigDone()) {
			Serial.print(".");
			// wiggle to show we are waiting for config from phone
			rotate(1 * STEPS_PER_MIN);
			rotate(-1 * STEPS_PER_MIN);
			vTaskDelay(pdMS_TO_TICKS(400));
		}
		if (WiFi.smartConfigDone()) {
			Serial.println("SmartConfig received.");
			Serial.println(String("host:") + WiFi.SSID());
			Serial.println(String("pass:") + WiFi.psk());
			strncpy(settings.cWifiID, WiFi.SSID().c_str(), sizeof(settings.cWifiID));
			strncpy(settings.cWifiPWD, WiFi.psk().c_str(), sizeof(settings.cWifiPWD));
			EEPROM.put(0, settings);
			EEPROM.commit();
			rotate(-5 * STEPS_PER_MIN);
			rotate(5 * STEPS_PER_MIN);
		}
		WiFi.stopSmartConfig();
		Serial.println();
	}
	if (settings.cWifiID[0] != '\0') {
		Serial.print(String("Connecting to network: ") + settings.cWifiID);
		WiFi.begin(settings.cWifiID, settings.cWifiPWD);
		// wait for the network
		int waitForNetwork = 4 * 60;
		while (waitForNetwork-- && WiFi.status() != WL_CONNECTED) {
			Serial.print(".");
			// wiggle the minutes while waiting for the network
			rotate(-STEPS_PER_MIN);
			rotate(STEPS_PER_MIN);
			vTaskDelay(pdMS_TO_TICKS(400));
		}
		// if no network wiggle 5 minutes back and forth
		if (WiFi.status() != WL_CONNECTED) {
			// running without a network
			rotate(-10 * STEPS_PER_MIN);
			rotate(10 * STEPS_PER_MIN);
			// timed out, so clear the network settings
			settings.cWifiID[0] = '\0';
			settings.cWifiPWD[0] = '\0';
			Serial.println("Network timed out, clearing network setting");
		}
		else {
			bWifiConnected = true;
		}
		Serial.println("");
		Serial.println(String("ip:") + WiFi.localIP().toString());
		configTime(settings.utcOffsetInSeconds, settings.bDST ? 3600 : 0, "pool.ntp.org");
		while (!getLocalTime(&gtime)) {
			vTaskDelay(1000);
		}
		Serial.println(&gtime, "%A, %B %d %Y %H:%M:%S");
		bGotTime = true;
		vTaskDelay(10);
		// get the epoch time, we'll use this later
		time_t et;
		time(&et);
	}
	// let the clock run anyway
	bGotTime = true;
	for (;;) {
        getLocalTime(&gtime);
		Serial.println("Time check : " + String(gtime.tm_hour) + ":" + gtime.tm_min);
		vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

/*
* handle the web server for adjusting settings
*/
void TaskServer(void* params)
{
	String header;
	unsigned long currentTime = millis();
	// Previous time
	unsigned long previousTime = 0;
	// Define timeout time in milliseconds (example: 2000ms = 2s)
	const long timeoutTime = 2000;
	// wait for WiFi to be ready
	while (!bWifiConnected)
		vTaskDelay(pdMS_TO_TICKS(1000));
	if (!MDNS.begin("hollowclock")) {   // Set the hostname to "hollowclock.local"
		Serial.println("Error setting up MDNS responder!");
	}
	server.begin();
	for (;;) {
		WiFiClient client = server.available();   // Listen for incoming clients

		if (client) {                             // If a new client connects,
			int adjustDST = 0;
			currentTime = millis();
			previousTime = currentTime;
			Serial.println("New Client.");          // print a message out in the serial port
			String currentLine = "";                // make a String to hold incoming data from the client
			while (client.connected() && currentTime - previousTime <= timeoutTime) {  // loop while the client's connected
				currentTime = millis();
				if (client.available()) {             // if there's bytes to read from the client,
					char c = client.read();             // read a byte, then
					Serial.write(c);                    // print it out the serial monitor
					header += c;
					if (c == '\n') {                    // if the byte is a newline character
						// if the current line is blank, you got two newline characters in a row.
						// that's the end of the client HTTP request, so send a response:
						if (currentLine.length() == 0) {
							// HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
							// and a content-type so the client knows what's coming, then a blank line:
							client.println("HTTP/1.1 200 OK");
							client.println("Content-type:text/html");
							client.println("Connection: close");
							client.println();
							//Serial.println(header);
							// turns DST on and off
							if (header.indexOf("GET /dst/on") >= 0) {
								settings.bDST = false;
								adjustDST = -1;
							}
							else if (header.indexOf("GET /dst/off") >= 0) {
								settings.bDST = true;
								adjustDST = 1;
							}
							else if (header.indexOf("GET /addminute") >= 0) {
								rotate(STEPS_PER_MIN);
							}
							else if (header.indexOf("GET /subminute") >= 0) {
								rotate(-STEPS_PER_MIN);
							}

							// Display the HTML web page
							client.println("<!DOCTYPE html><html>");
							client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
							client.println("<link rel=\"icon\" href=\"data:,\">");
							// CSS to style the on/off buttons 
							// Feel free to change the background-color and font-size attributes to fit your preferences
							client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
							client.println(".button { background-color: #4CAF50; border: none; color: white; padding: 16px 40px;");
							client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
							client.println(".button2 {background-color: #555555;}</style></head>");

							// Web Page Heading
							client.println("<body><h1>Hollow Clock</h1>");

							client.println(String("<p>Time ") + gtime.tm_hour + ":" + gtime.tm_min + "</p>");
							// Display current state, and ON/OFF buttons for DST 
							client.println(String("<p>DST is ") + (settings.bDST ? "on" : "off") + "</p>");
							// If the DST is off, it displays the ON button       
							if (settings.bDST) {
								client.println("<p><a href=\"/dst/on\"><button class=\"button\">ON</button></a></p>");
							}
							else {
								client.println("<p><a href=\"/dst/off\"><button class=\"button button2\">OFF</button></a></p>");
							}
							client.println(String("<p>UTC = ") + settings.utcOffsetInSeconds / 60 / 60 + "</p>");
							client.println("<p><a href=\"/addminute\"><button class=\"button\">Add Minute</button></a></p>");
							client.println("<p><a href=\"/subminute\"><button class=\"button\">Subtract Minute</button></a></p>");

							client.println("</body></html>");

							// The HTTP response ends with another blank line
							client.println();
							// Break out of the while loop
							break;
						}
						else { // if you got a newline, then clear currentLine
							currentLine = "";
						}
					}
					else if (c != '\r') {  // if you got anything else but a carriage return character,
						currentLine += c;      // add it to the end of the currentLine
					}
				}
			}
			// Clear the header variable
			header = "";
			// Close the connection
			client.stop();
			Serial.println("Client disconnected.");
			Serial.println("");
			// adjust DST if necessary
			//if (adjustDST == 1)
			//	rotate(60 * STEPS_PER_MIN);
			//else if (adjustDST == -1)
			//	rotate(-60 * STEPS_PER_MIN);
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void setup()
{
    pinMode(port[0], OUTPUT);
    pinMode(port[1], OUTPUT);
    pinMode(port[2], OUTPUT);
    pinMode(port[3], OUTPUT);
	delay(500);
	Serial.begin(115200);
    while (!Serial.availableForWrite()) {
        delay(10);
    }
    // need delay or it won't write after upload
    delay(500);
    Serial.println("clock starting");
    MutexRotateHandle = xSemaphoreCreateMutex();
    EEPROM.begin(512);
    int checkVer;
    EEPROM.get(0, checkVer);
    // see if value is valid
    if (checkVer == HC_VERSION) {
        EEPROM.get(0, settings);
        Serial.println("Loaded settings");
    }
    else {
        // invalid value, so save defaults
        EEPROM.put(0, settings);
        EEPROM.commit();
        Serial.println("Loaded default settings");
    }
    xTaskCreate(TaskMinutes, "MINUTES", 1000, NULL, 3, &TaskClockMinuteHandle);
    xTaskCreate(TaskMenu, "MENU", 9000, NULL, 6, &TaskMenuHandle);
    xTaskCreate(TaskWiFi, "WIFI", 4000, NULL, 2, &TaskWifiHandle);
	xTaskCreate(TaskServer, "SERVER", 10000, NULL, 5, &TaskServerHandle);
}

void loop()
{
    if (settings.bTestMode) {
        // just run the motor
		rotate(60 * STEPS_PER_MIN);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
	//Serial.println(String("mins: ") + uxTaskGetStackHighWaterMark(TaskClockMinuteHandle));
	//Serial.println(String("menu: ") + uxTaskGetStackHighWaterMark(TaskMenuHandle));
	//Serial.println(String("wifi: ") + uxTaskGetStackHighWaterMark(TaskWifiHandle));
	//Serial.println(String("srvr: ") + uxTaskGetStackHighWaterMark(TaskServerHandle));
}

/*
 Name:		HollowClockEsp32.ino
 Created:	7/25/2024 2:28:42 PM
 Author:	Martin Nohr
*/
#include <EEPROM.h>
#include <freertos.h>
#include <stdio.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include <FS.h>
#include <SPIFFS.h>

SemaphoreHandle_t MutexRotateHandle;

#define HC_VERSION 2  // change this when the settings structure is changed

// Motor and clock parameters
// 2048 * 90 / 12 / 60 = 256
#define STEPS_PER_MIN 256

// hold setting information in EEPROM
struct {
    int nVersion = HC_VERSION;  // this int must be first
    bool bReverse = false;
    bool bTestMode = false;
    int nStepSpeed = 2;
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
			vTaskDelay(dt);
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
time_t g_NetTime;
time_t g_ClockTime;
unsigned long g_nUptimeMinutes;

void TaskMinutes(void* params)
{
    // use this to make task run every second
    TickType_t xLastWakeTime;
	const TickType_t xFrequency = pdMS_TO_TICKS(1000 * 60);
    // Initialize the xLastWakeTime variable with the current time.
    xLastWakeTime = xTaskGetTickCount();
    Serial.println("waiting for internet time");
    // wait for WiFi time to be ready
    while (!bGotTime) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
	Serial.println("got internet time, setting clock");
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
	//portMUX_TYPE mux;
	//portMUX_INITIALIZE(&mux);
	Serial.println("starting minute task");
    for (;;) {
		static char line[100];
		//vPortEnterCritical(&mux);
		//portDISABLE_INTERRUPTS();
		//sprintf(line, "running time (d:hr:mn) : %lu:%02lu:%02lu", g_nUptimeMinutes / 60 / 24, g_nUptimeMinutes / 60, g_nUptimeMinutes % 60);
		//Serial.println(line);
		//Serial.printf("clock time:%lld net time:%lld\n", g_ClockTime, g_NetTime);
		//vPortExitCritical(&mux);
		//portENABLE_INTERRUPTS();
		if (!settings.bTestMode) {
			rotate(STEPS_PER_MIN);
		}
		// Wait for the next cycle.
		vTaskDelayUntil(&xLastWakeTime, xFrequency);
		++g_nUptimeMinutes;
		// increment clock time by 1 minute
		g_ClockTime += 60;
	}
}

void TaskMenu(void* params)
{
	Serial.println("? for menu");
	for (;;) {
		if (Serial.available()) {
			int argval = 0;
			String str;
			str = Serial.readString();
			str.trim();
			//Serial.println("Received:" + str);
			bool bSave = false;
			char ch = str[0];
			str = str.substring(1);
			str.trim();
			argval = str.toInt();
			switch (toupper(ch)) {
			case 'N':
				if (str.length()) {
					strncpy(settings.cWifiID, str.c_str(), sizeof(settings.cWifiID) - 1);
					Serial.printf("Network Name: %s", str.c_str());
					bSave = true;
				}
				break;
			case 'P':
				if (str.length()) {
					strncpy(settings.cWifiPWD, str.c_str(), sizeof(settings.cWifiPWD) - 1);
					Serial.printf("Password: %s", str.c_str());
					bSave = true;
				}
				break;
			case 'C':
				settings.cWifiID[0] = '\0';
				settings.cWifiPWD[0] = '\0';
				bSave = true;
				break;
			case 'T':
				settings.bTestMode = !settings.bTestMode;
				bSave = true;
				break;
			case 'U':
				settings.utcOffsetInSeconds = str.toInt() * 3600;
				bSave = true;
				break;
			case 'D':
				settings.bDST = !settings.bDST;
				// adjust the time
				rotate((settings.bDST ? 1 : -1) * 60 * STEPS_PER_MIN);
				bSave = true;
				g_ClockTime = g_NetTime;
				break;
			case 'S':
				settings.nStepSpeed = str.toInt();
				bSave = true;
				break;
			case 'A':  // adjust stepper position
				if (argval == 0)
					argval = 1;
				rotate(argval);
				// if negative move backwards to take up slack
				if (argval < 0) {
					rotate(-STEPS_PER_MIN);
					rotate(STEPS_PER_MIN);
				}
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
			struct tm *localT;
			localT = localtime(&g_ClockTime);
			Serial.println("---------------------------");
			Serial.print("Last Sync  : ");
			Serial.println(&gtime, "%A, %B %d %Y %H:%M:%S");
			Serial.print("Clock Time : ");
			Serial.println(localT, "%A, %B %d %Y %H:%M:%S");
			Serial.println(String("Network    : ") + settings.cWifiID);
			Serial.println(String("Password   : ") + settings.cWifiPWD);
			Serial.println(String("UTC        : ") + (settings.utcOffsetInSeconds / 3600));
			Serial.println(String("DST        : ") + (settings.bDST ? "ON" : "OFF"));
			Serial.println(String("Step Delay : ") + settings.nStepSpeed + " mS");
			Serial.println(String("Test mode  : ") + (settings.bTestMode ? "ON" : "OFF"));
			Serial.println("---------------------------");
			Serial.println("N<networkID>  = network name (case sensitive)");
			Serial.println("P<password>   = password for network (case sensitive)");
			Serial.println("C             = clear network name and password");
			Serial.println("U<-12 to +12> = utc offset in hours");
			Serial.println("D             = toggle daylight saving (DST)");
			Serial.println("A<n>          = Adjust Minute Position (+/- 256 is a full minute)");
			Serial.println("S<2 to 10>    = stepper delay in mS");
			Serial.println("T             = toggle test mode");
			Serial.println("+<n>          = add one or more minutes");
			Serial.println("-<n>          = subtract one or more minutes");
			Serial.println("Command? ");
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
	// check the time once per day TODO: change * 1 to * 24
	const TickType_t xFrequency = pdMS_TO_TICKS(1000) * 60 * 60 * 1;
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
			vTaskDelay(pdMS_TO_TICKS(250));
		}
		// if no network go 10 minutes back and forth
		if (WiFi.status() != WL_CONNECTED) {
			// running without a network
			rotate(-10 * STEPS_PER_MIN);
			rotate(10 * STEPS_PER_MIN);
			// timed out, so clear the network settings
			settings.cWifiID[0] = '\0';
			settings.cWifiPWD[0] = '\0';
			EEPROM.put(0, settings);
			EEPROM.commit();
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
		// get our two times for tracking and correction as needed
		time(&g_ClockTime);
		g_NetTime = g_ClockTime;
		Serial.println(&gtime, "%A, %B %d %Y %H:%M:%S");
		bGotTime = true;
		vTaskDelay(10);
	}
	// let the clock run anyway
	bGotTime = true;
	for (;;) {
        getLocalTime(&gtime);
		time(&g_NetTime);
		Serial.printf("Time value : %02d:%02d:%02d\n\r", gtime.tm_hour, gtime.tm_min, gtime.tm_sec);
		Serial.printf("Clock Time:%lld Net Time:%lld diff:%lld\n", g_ClockTime, g_NetTime, (g_ClockTime - g_NetTime));
		int diffMinutes = (g_ClockTime - g_NetTime) / 60;
		if (diffMinutes > 0) {
			// too fast, backup
			rotate(-diffMinutes * STEPS_PER_MIN);
			g_ClockTime = g_NetTime;
			Serial.println(String("Clock adjusted back by: ") + diffMinutes);
		}
		else if (diffMinutes < 0) {
			// too slow, speed up
			rotate(diffMinutes * STEPS_PER_MIN);
			g_ClockTime = g_NetTime;
			Serial.println(String("Clock adjusted forward by: ") + diffMinutes);
		}
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
	else {
		Serial.println("MDNS: hollowclock.local");
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

							client.println(String("<p>Last Net Sync Time ") + gtime.tm_hour + ":" + gtime.tm_min + "</p>");
							struct tm* localT;
							localT = localtime(&g_ClockTime);
							client.println(String("<p>Clock Time ") + localT->tm_hour + ":" + localT->tm_min + "</p>");
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
							client.println("<p>Adjust Minutes&nbsp;<a href=\"/addminute\"><button class=\"button\">+1</button></a>&nbsp;");
							client.println("<a href = \"/subminute\"><button class=\"button\">-1</button></a></p>");

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
			vTaskDelay(pdMS_TO_TICKS(100));
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void listDir(fs::FS& fs, const char* dirname, uint8_t levels)
{
	Serial.printf("Listing directory: %s\r\n", dirname);
	File root = fs.open(dirname);
	if (!root) {
		Serial.println("- failed to open directory");
		return;
	}
	if (!root.isDirectory()) {
		Serial.println(" - not a directory");
		return;
	}
	File file = root.openNextFile();
	while (file) {
		if (file.isDirectory()) {
			Serial.print("  DIR : ");
			Serial.println(file.name());
			if (levels) {
				listDir(fs, file.name(), levels - 1);
			}
		}
		else {
			Serial.print("  FILE: ");
			Serial.print(file.name());
			Serial.print("\tSIZE: ");
			Serial.println(file.size());
		}
		file = root.openNextFile();
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
	//if (!SPIFFS.begin(true)) {
	//	Serial.println("spiffs failed");
	//}
	//listDir(SPIFFS, "/", 0);
	//char* path = "/HomePage.html";
	//Serial.printf("Reading file: %s\r\n", path);
	//File file = SPIFFS.open(path);
	//if (!file || file.isDirectory()) {
	//	Serial.println("- failed to open file for reading");
	//	return;
	//}
	//Serial.println("- read from file:");
	//while (file.available()) {
	//	Serial.write(file.readString().c_str());
	//}
	//file.close();
	xTaskCreatePinnedToCore(TaskMinutes, "MINUTES", 9000, NULL, 1, &TaskClockMinuteHandle, 1);
	xTaskCreatePinnedToCore(TaskMenu, "MENU", 9000, NULL, 5, &TaskMenuHandle, 1);
	xTaskCreatePinnedToCore(TaskWiFi, "WIFI", 4000, NULL, 2, &TaskWifiHandle, 1);
	xTaskCreatePinnedToCore(TaskServer, "SERVER", 10000, NULL, 5, &TaskServerHandle, 1);
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

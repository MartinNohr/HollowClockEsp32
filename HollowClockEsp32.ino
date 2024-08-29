/*
 Name:		HollowClockEsp32.ino
 Created:	7/25/2024 2:28:42 PM
 Author:	Martin Nohr
*/
#include <EEPROM.h>
#include <freertos.h>
#include <stdio.h>
#include <wifi.h>
#include <time.h>
SemaphoreHandle_t MutexRotateHandle;

#define HC_VERSION 0  // change this when the settings structure is changed

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
    char cWifiID[20] = "NohrNet";
    char cWifiPWD[20] = "8017078120";
    long utcOffsetInSeconds = -7 * 3600;
} settings;

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
volatile bool bGotTime = false;
struct tm gtime;

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
    // calculate how far to move the clock from noon
    int howfar = (gtime.tm_hour % 12) * 60 + gtime.tm_min;
    //Serial.println(String("goto time: ") + howfar);
    // if after 6 take the shorter route in reverse
    if (howfar > 6 * 60) {
        howfar -= 12 * 60;
    }
    // assume starting at noon
    rotate(STEPS_PER_MIN * howfar);
    for (;;) {
		if (!settings.bTestMode) {
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
			bool bSave = false;
			switch (toupper(str[0])) {
			case '?':
				Serial.println("---------------------------");
				Serial.println(String("Network   : ") + settings.cWifiID);
				Serial.println(String("Password  : ") + settings.cWifiPWD);
				Serial.println(String("test mode : ") + settings.bTestMode);
				Serial.println(String("UTC       : ") + (settings.utcOffsetInSeconds / 3600));
				Serial.println(String("Delay     : ") + settings.nStepSpeed);
				Serial.println("---------------------------");
				Serial.println("N<networkID>  = network name (case sensitive)");
				Serial.println("P<password>   = password for network (case sensitive)");
				Serial.println("T             = toggle test mode");
				Serial.println("U<-12 to +12> = utc offset in hours");
				Serial.println("D<2 to 10>    = stepper delay in mS");
				Serial.println();
				break;
			case 'N':
				str = str.substring(1);
				str.trim();
				if (str.length()) {
					strncpy(settings.cWifiID, str.c_str(), sizeof(settings.cWifiID) - 1);
					Serial.println("Network Name:" + str);
				}
				bSave = true;
				break;
			case 'P':
				str = str.substring(1);
				str.trim();
				if (str.length()) {
					strncpy(settings.cWifiPWD, str.c_str(), sizeof(settings.cWifiPWD) - 1);
					Serial.println("Password:" + str);
				}
				bSave = true;
				break;
			case 'T':
				settings.bTestMode = !settings.bTestMode;
				bSave = true;
				break;
			case 'U':
				str = str.substring(1);
				str.trim();
				settings.utcOffsetInSeconds = str.toInt() * 3600;
				bSave = true;
				break;
			case 'D':
				str = str.substring(1);
				str.trim();
				settings.nStepSpeed = str.toInt();
				bSave = true;
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

void TaskWiFi(void* params)
{
    // init the WiFi
	Serial.print(String("Connecting to network: ") + settings.cWifiID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(settings.cWifiID, settings.cWifiPWD);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(1000);
    }
    Serial.println("");
	Serial.println(String("ip:") + WiFi.localIP().toString());
    configTime(settings.utcOffsetInSeconds, 3600, "north-america.pool.ntp.org");
    getLocalTime(&gtime);
    Serial.println(&gtime, "%A, %B %d %Y %H:%M:%S");
    bGotTime = true;
    vTaskDelay(10);
    for (;;) {
        getLocalTime(&gtime);
        Serial.println(String(gtime.tm_hour) + ":" + gtime.tm_min);
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void setup()
{
    pinMode(port[0], OUTPUT);
    pinMode(port[1], OUTPUT);
    pinMode(port[2], OUTPUT);
    pinMode(port[3], OUTPUT);
	delay(500);
	Serial.begin(9600);
    while (!Serial.availableForWrite()) {
        delay(10);
    }
    // need delay or it won't write after upload
    delay(500);
    Serial.println("clock starting");
    MutexRotateHandle = xSemaphoreCreateMutex();
    //rotate(-STEPS_PER_MIN * 2); // initialize
    //rotate(STEPS_PER_MIN * 1);
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
    xTaskCreate(TaskMenu, "MENU", 2000, NULL, 4, &TaskMenuHandle);
    xTaskCreate(TaskWiFi, "WIFI", 4000, NULL, 2, &TaskWifiHandle);
}

void loop()
{
    if (settings.bTestMode) {
        // just run the motor
        rotate(1000);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

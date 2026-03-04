# http_parameters

Minimal ESP32 HTTP parameter editor backed by LittleFS.

## Dependencies

- Platform: `espressif32`
- Framework: `arduino`
- Libraries used by this module:
  - `WiFi`
  - `WebServer`
  - `LittleFS`
  - `ArduinoJson`

## What `http_parameters.hpp` provides

- In-memory parameter map with LittleFS persistence (`/http_params.json`)
- Built-in HTTP page to view/edit parameters
- Endpoints:
  - `GET /` render form
  - `POST /submit` save values
  - `POST /clear` clear stored values

## Include

```cpp
#include "http_parameters.hpp"
```

The global instance `Http_parameters` is already provided by the header.

## Quick usage

```cpp
#include "http_parameters.hpp"
#include <WiFi.h>

void setup() {
	WiFi.begin("SSID", "PASSWORD");
	while (WiFi.status() != WL_CONNECTED) {
		delay(200);
	}

	Http_parameters.set("var1", "default1");
	Http_parameters.set("var2", "default2");

	Http_parameters.begin(
		[]() { Serial.println("Saved"); },
		nullptr,
		[]() { ESP.restart(); }
	);
}

void loop() {
	Http_parameters.handleClient();
}
```

## Runtime helpers

- `Http_parameters.get(key, fallback)`
- `Http_parameters.set(key, value, override)`
- `Http_parameters.has(key)`
- `Http_parameters.clear()`

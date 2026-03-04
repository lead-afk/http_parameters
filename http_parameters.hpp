#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Webserver.h>
#include <functional>
#include <json_parser.h>
#include <map>

/**
 * @brief Small HTTP-backed parameter store persisted in LittleFS.
 *
 * Exposes a web page for editing key/value parameters and helper methods
 * to read, set, clear, and persist values at runtime.
 */
class Http_parameters_
{
private:
    /**
     * @brief RAII guard for entering/leaving the map critical section.
     */
    struct map_lock_guard
    {
        portMUX_TYPE* mux;

        /**
         * @brief Enter critical section for the provided port mutex.
         * @param m Pointer to lock used for shared map access.
         */
        explicit map_lock_guard(portMUX_TYPE* m) : mux(m)
        {
            portENTER_CRITICAL(mux);
        }

        /**
         * @brief Leave critical section.
         */
        ~map_lock_guard()
        {
            portEXIT_CRITICAL(mux);
        }
    };

    static std::map<String, String> parameters;
    static bool parametersLoaded;
    static portMUX_TYPE parametersLock;

    /**
     * @brief Mount LittleFS, formatting if mount fails.
     * @return true when filesystem is mounted and ready.
     */
    static bool mountLittleFS()
    {
        if (LittleFS.begin(true))
        {
            return true;
        }

        Serial.println("Failed to mount LittleFS (even after format).");
        return false;
    }

    /**
     * @brief Load parameter map from /http_params.json once.
     */
    static void loadParameters()
    {
        if (parametersLoaded)
        {
            return;
        }

        if (!mountLittleFS())
        {
            parametersLoaded = true;
            return;
        }

        if (LittleFS.exists("/http_params.json"))
        {
            File configFile = LittleFS.open("/http_params.json", "r");
            if (configFile)
            {
                String jsonString = configFile.readString();
                configFile.close();

                JsonDocument parser;
                DeserializationError error = deserializeJson(parser, jsonString);
                if (!error)
                {
                    map_lock_guard lock(&parametersLock);
                    for (JsonPair kv : parser.as<JsonObject>())
                    {
                        parameters[kv.key().c_str()] = kv.value().as<String>();
                    }
                }
            }
        }
        else
        {
            Serial.println("No config file found.");
            File configFile = LittleFS.open("/http_params.json", "w");
            if (configFile)
            {
                configFile.print("{}");
                configFile.close();
            }
        }
        LittleFS.end();
        parametersLoaded = true;
    }

    /**
     * @brief Ensure in-memory parameter map is initialized.
     */
    static void ensureLoaded()
    {
        if (!parametersLoaded)
        {
            loadParameters();
            map_lock_guard lock(&parametersLock);
            if (parameters.find("http_parameters_server_port") == parameters.end())
            {
                parameters["http_parameters_server_port"] = "80";
            }
            if (parameters.find("http_parameters_auth_username") == parameters.end())
            {
                parameters["http_parameters_auth_username"] = "admin";
            }
            if (parameters.find("http_parameters_auth_password") == parameters.end())
            {
                parameters["http_parameters_auth_password"] = "admin";
            }
        }
    }

    /**
     * @brief Read a parameter value by key.
     * @param key Parameter name.
     * @param default_value Value returned when key is missing.
     * @return Stored value or default.
     */
    static String getParameter(const String& key, const String& default_value)
    {
        ensureLoaded();
        {
            map_lock_guard lock(&parametersLock);
            auto it = parameters.find(key);
            if (it != parameters.end())
            {
                return it->second;
            }
        }

        if (!mountLittleFS())
        {
            return default_value;
        }

        if (LittleFS.exists("/http_params.json"))
        {
            File configFile = LittleFS.open("/http_params.json", "r");
            if (configFile)
            {
                String jsonString = configFile.readString();
                configFile.close();

                JsonDocument parser;
                DeserializationError error = deserializeJson(parser, jsonString);
                if (!error)
                {
                    if (parser[key].is<String>())
                    {
                        return parser[key].as<String>();
                    }
                }
            }
        }
        else
        {
            Serial.println("No config file found.");
            File configFile = LittleFS.open("/http_params.json", "w");
            if (configFile)
            {
                configFile.print("{}");
                configFile.close();
            }
        }

        LittleFS.end();
        return default_value;
    }

    /**
     * @brief Persist all current parameters to /http_params.json.
     */
    static void saveParameters()
    {
        ensureLoaded();
        std::map<String, String> snapshot;
        {
            map_lock_guard lock(&parametersLock);
            snapshot = parameters;
        }

        JsonDocument doc;
        for (const auto& pair : snapshot)
        {
            doc[pair.first] = pair.second;
        }

        String jsonString;
        serializeJson(doc, jsonString);

        if (!mountLittleFS())
        {
            return;
        }

        File configFile = LittleFS.open("/http_params.json", "w");
        if (configFile)
        {
            configFile.print(jsonString);
            configFile.close();
        }
        LittleFS.end();
    }

    /**
     * @brief Set a parameter and optionally persist to flash.
     * @param key Parameter name.
     * @param value Parameter value.
     * @param save When true, writes map to LittleFS.
     */
    static void setParameter(const String& key, const String& value, bool save = true)
    {
        ensureLoaded();
        {
            map_lock_guard lock(&parametersLock);
            parameters[key] = value;
        }
        if (save)
        {
            saveParameters();
        }
    }

    WebServer* server = nullptr;

    bool ensureAuthenticated()
    {
        String username = get("http_parameters_auth_username", "admin");
        String password = get("http_parameters_auth_password", "admin");

        if (username.isEmpty() && password.isEmpty())
        {
            return true;
        }

        if (!server->authenticate(username.c_str(), password.c_str()))
        {
            server->requestAuthentication();
            return false;
        }

        return true;
    }

    /**
     * @brief Handle GET / and render editable HTML form.
     */
    void handleRoot()
    {

        if (!ensureAuthenticated())
        {
            return;
        }

        std::vector<String> keys;
        String html = "<!doctype html><html><head><meta charset='utf-8'><title>HTTP Parameters</title></head>"
                      "<body><h1>HTTP Parameters</h1>"
                      "<form id='dataForm'>";
        html += additionalHtmlCallback ? additionalHtmlCallback() : "";
        {
            ensureLoaded();
            map_lock_guard lock(&parametersLock);
            for (const auto& pair : parameters)
            {
                keys.push_back(pair.first);
            }

            for (int i = 0; i < keys.size(); i++)
            {
                String key = keys[i];
                String value = parameters[key];

                html += "<label for='" + key + "'>" + key + "</label><br/>";
                html += "<input type='text' id='" + key + "' name='" + key + "' value='" + value +
                        "' placeholder='Type " + key + "' /><br/><br/>";
            }
        }

        html += "<button type='button' id='saveBtn'>Save</button>";
        html += "<br/><br/>";
        html += "<button type='button' id='clearBtn'>Clear Parameters</button>";
        html += "<br/><br/>";
        html += "<button type='button' id='restartBtn'>Restart ESP32</button>";
        html += "<br/><br/><div id='status'></div>";
        html += "</form>";
        html += "<script>"
                "async function postForm(action){"
                "const form=document.getElementById('dataForm');"
                "const status=document.getElementById('status');"
                "const body=new URLSearchParams(new FormData(form));"
                "body.set('action',action);"
                "status.textContent='sending...';"
                "try{"
                "const res=await "
                "fetch('/submit',{method:'POST',headers:{'Content-Type':'application/"
                "x-www-form-urlencoded'},body:body.toString()});"
                "if(!res.ok){throw new Error('http');}"
                "status.textContent= action==='save' ? 'ok' : 'restarting...';"
                "}catch(e){status.textContent='failed';}"
                "}"
                "async function clearParams(){"
                "const status=document.getElementById('status');"
                "status.textContent='clearing...';"
                "try{"
                "const res=await fetch('/clear',{method:'POST'});"
                "if(!res.ok){throw new Error('http');}"
                "status.textContent='cleared';"
                "setTimeout(function(){location.reload();},300);"
                "}catch(e){status.textContent='failed';}"
                "}"
                "document.getElementById('saveBtn').addEventListener('click',function(){postForm('save');});"
                "document.getElementById('clearBtn').addEventListener('click',clearParams);"
                "document.getElementById('restartBtn').addEventListener('click',function(){postForm('restart');});"
                "</script></body></html>";

        server->send(200, "text/html", html);
    }

    /**
     * @brief Handle POST /submit, update parameters, and reply with JSON.
     */
    void handleSubmit()
    {
        if (!ensureAuthenticated())
        {
            return;
        }

        std::vector<String> keys;
        JsonDocument response;

        ensureLoaded();

        {
            map_lock_guard lock(&parametersLock);
            for (const auto& pair : parameters)
            {
                keys.push_back(pair.first);
            }
        }

        Serial.println("Received form data:");

        for (int i = 0; i < keys.size(); i++)
        {
            String key = keys[i];
            String value = server->arg(key);

            Serial.print(key);
            Serial.print(": ");
            Serial.println(value);

            setParameter(key, value, false);

            response[key] = value;
        }

        String action = server->arg("action");

        response["action"] = action;

        String json;
        serializeJson(response, json);

        saveParameters();

        if (onSaveCallback)
        {
            onSaveCallback();
        }

        server->send(200, "application/json", json);

        if (action == "restart")
        {
            if (onRestartCallback)
            {
                onRestartCallback();
            }
            else
            {
                Serial.println("Restart was called but no callback is set, restarting immediately...");
                ESP.restart();
            }
        }
    }

    /**
     * @brief Handle POST /clear and remove stored parameters.
     */
    void handleClear()
    {
        if (!ensureAuthenticated())
        {
            return;
        }

        clear();
        server->send(200, "text/plain", "Parameters cleared");
    }

    std::function<void(void)> onSaveCallback;
    std::function<void(void)> onRestartCallback;
    std::function<String(void)> additionalHtmlCallback;

    /**
     * @brief Read and validate configured server port.
     * @return Port number in range 1..65535, defaulting to 80.
     */
    static int getDefaultServerPort()
    {
        int port = getParameter("http_parameters_server_port", "80").toInt();
        if (port <= 0 || port > 65535)
        {
            port = 80;
        }
        return port;
    }

public:
    /**
     * @brief Construct server using configured or default port.
     */
    Http_parameters_() {

    };

    /**
     * @brief Get parameter by key.
     * @param key Parameter name.
     * @param default_value Fallback when key is not found.
     * @return Stored value or fallback value.
     */
    String get(const String& key, const String& default_value = "")
    {
        return getParameter(key, default_value);
    }

    /**
     * @brief Set parameter when missing, or force overwrite when override=true.
     * @param key Parameter name.
     * @param default_value Value to write.
     * @param override Overwrite existing value when true.
     */
    void set(const String& key, const String& default_value, const bool override = false)
    {
        ensureLoaded();
        bool exists = false;
        {
            map_lock_guard lock(&parametersLock);
            exists = parameters.find(key) != parameters.end();
        }

        if (override || !exists)
        {
            setParameter(key, default_value);
        }
    }

    /**
     * @brief Check if a parameter key exists.
     * @param key Parameter name.
     * @return true when key exists in the map.
     */
    bool has(const String& key)
    {
        ensureLoaded();
        map_lock_guard lock(&parametersLock);
        return parameters.find(key) != parameters.end();
    }

    /**
     * @brief Start HTTP server and register routes.
     * @param onSave Optional callback after save action.
     * @param additional_html Optional extra HTML inserted in form.
     * @param onRestart Optional restart callback for restart action.
     */
    void begin(
        std::function<void(void)> onSave = nullptr, std::function<String(void)> additional_html = nullptr,
        std::function<void(void)> onRestart = []() { ESP.restart(); })
    {
        ensureLoaded();

        int port = getDefaultServerPort();
        server = new WebServer(port);

        onSaveCallback = onSave;
        onRestartCallback = onRestart;
        additionalHtmlCallback = additional_html;

        server->on("/", HTTP_GET, std::bind(&Http_parameters_::handleRoot, this));
        server->on("/submit", HTTP_POST, std::bind(&Http_parameters_::handleSubmit, this));
        server->on("/clear", HTTP_POST, std::bind(&Http_parameters_::handleClear, this));
        server->onNotFound(std::bind(&Http_parameters_::handleRoot, this));
        server->begin();
    }

    /**
     * @brief Clear all parameters and recreate default server port.
     */
    void clear()
    {
        Serial.println("Clearing parameters...");
        ensureLoaded();
        {
            map_lock_guard lock(&parametersLock);
            parameters.clear();
            parameters["http_parameters_server_port"] = "80";
            parameters["http_parameters_auth_username"] = "admin";
            parameters["http_parameters_auth_password"] = "admin";
        }
        saveParameters();
    }

    /**
     * @brief Process pending HTTP client requests.
     */
    void handleClient()
    {
        if (server)
        {
            server->handleClient();
        }
        else
        {
            Serial.println("[Server] not initialized!");
            Serial.println(
                "Remember to call Http_parameters.begin() in setup() before calling handleClient() in loop().");
        }
    }

    void setLoginPassword(const String& username, const String& password)
    {
        set("http_parameters_auth_username", username, true);
        set("http_parameters_auth_password", password, true);
    }

    /**
     * @brief Stop HTTP server->
     */
    ~Http_parameters_()
    {
        server->stop();
        delete server;
    };
};

inline std::map<String, String> Http_parameters_::parameters = {};
inline bool Http_parameters_::parametersLoaded = false;
inline portMUX_TYPE Http_parameters_::parametersLock = portMUX_INITIALIZER_UNLOCKED;

static Http_parameters_ Http_parameters;
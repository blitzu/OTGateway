#include <CustomOpenTherm.h>

CustomOpenTherm* ot;
extern FileData fsSettings;

class OpenThermTask : public Task {
public:
  OpenThermTask(bool _enabled = false, unsigned long _interval = 0) : Task(_enabled, _interval) {
    ot = new CustomOpenTherm(settings.opentherm.inPin, settings.opentherm.outPin);
  }

  static void IRAM_ATTR handleInterrupt() {
    ot->handleInterrupt();
  }

protected:
  unsigned short readyTime = 60000;
  unsigned short dhwSetTempInterval = 60000;
  unsigned short heatingSetTempInterval = 60000;

  bool pump = true;
  unsigned long lastSuccessResponse = 0;
  unsigned long prevUpdateNonEssentialVars = 0;
  unsigned long startupTime = millis();
  unsigned long dhwSetTempTime = 0;
  unsigned long heatingSetTempTime = 0;
  byte dhwFlowRateMultiplier = 1;
  byte pressureMultiplier = 1;

  const char* getTaskName() {
    return "OpenTherm";
  }
  
  int getTaskCore() {
    return 1;
  }

  int getTaskPriority() {
    return 2;
  }

  void setup() {
    Log.sinfoln(FPSTR(L_OT), F("Started. GPIO IN: %hhu, GPIO OUT: %hhu"), settings.opentherm.inPin, settings.opentherm.outPin);

    ot->setAfterSendRequestCallback([this](unsigned long request, unsigned long response, OpenThermResponseStatus status, byte attempt) {
      Log.straceln(
        FPSTR(L_OT),
        F("ID: %4d   Request: %8lx   Response: %8lx   Attempt: %2d   Status: %s"),
        ot->getDataID(request), request, response, attempt, ot->statusToString(status)
      );

      if (status == OpenThermResponseStatus::SUCCESS) {
        this->lastSuccessResponse = millis();

        #ifdef LED_OT_RX_PIN
        {
          digitalWrite(LED_OT_RX_PIN, HIGH);
          delayMicroseconds(2000);
          digitalWrite(LED_OT_RX_PIN, LOW);
        }
        #endif
      }
    });

    ot->setYieldCallback([this]() {
      this->delay(25);
    });

    ot->setMinWaitTimeForStartBit(20000);
    ot->begin(OpenThermTask::handleInterrupt);

    #ifdef LED_OT_RX_PIN
      pinMode(LED_OT_RX_PIN, OUTPUT);
      digitalWrite(LED_OT_RX_PIN, LOW);
    #endif
  }

  void initBoiler() {
    this->dhwFlowRateMultiplier = 1;
    this->pressureMultiplier = 1;

    // Not all boilers support these, only try once when the boiler becomes connected
    if (updateSlaveVersion()) {
      Log.straceln(FPSTR(L_OT), F("Slave version: %u, type: %u"), vars.parameters.slaveVersion, vars.parameters.slaveType);

    } else {
      Log.swarningln(FPSTR(L_OT), F("Get slave version failed"));
    }

    // 0x013F
    if (setMasterVersion(0x3F, 0x01)) {
      Log.straceln(FPSTR(L_OT), F("Master version: %u, type: %u"), vars.parameters.masterVersion, vars.parameters.masterType);
      
    } else {
      Log.swarningln(FPSTR(L_OT), F("Set master version failed"));
    }

    if (updateSlaveConfig()) {
      Log.straceln(FPSTR(L_OT), F("Slave member id: %u, flags: %u"), vars.parameters.slaveMemberId, vars.parameters.slaveFlags);

    } else {
      Log.swarningln(FPSTR(L_OT), F("Get slave config failed"));
    }

    if (setMasterConfig(settings.opentherm.memberIdCode & 0xFF, (settings.opentherm.memberIdCode & 0xFFFF) >> 8)) {
      Log.straceln(FPSTR(L_OT), F("Master member id: %u, flags: %u"), vars.parameters.masterMemberId, vars.parameters.masterFlags);
      
    } else {
      Log.swarningln(FPSTR(L_OT), F("Set master config failed"));
    }
  }

  void loop() {
    static byte currentHeatingTemp, currentDhwTemp = 0;
    unsigned long localResponse;

    bool heatingEnabled = (vars.states.emergency || settings.heating.enable) && this->pump && isReady();
    bool heatingCh2Enabled = settings.opentherm.heatingCh2Enabled;
    if (settings.opentherm.heatingCh1ToCh2) {
      heatingCh2Enabled = heatingEnabled;

    } else if (settings.opentherm.dhwToCh2) {
      heatingCh2Enabled = settings.opentherm.dhwPresent && settings.dhw.enable;
    }

    localResponse = ot->setBoilerStatus(
      heatingEnabled,
      settings.opentherm.dhwPresent && settings.dhw.enable,
      false,
      false,
      heatingCh2Enabled,
      settings.opentherm.summerWinterMode,
      settings.opentherm.dhwBlocking
    );

    if (!ot->isValidResponse(localResponse)) {
      Log.swarningln(FPSTR(L_OT), F("Invalid response after setBoilerStatus: %s"), ot->statusToString(ot->getLastResponseStatus()));
    }

    if (!vars.states.otStatus && millis() - this->lastSuccessResponse < 1150) {
      Log.sinfoln(FPSTR(L_OT), F("Connected. Initializing"));
      
      vars.states.otStatus = true;
      this->initBoiler();
      
    } else if (vars.states.otStatus && millis() - this->lastSuccessResponse > 1150) {
      Log.swarningln(FPSTR(L_OT), F("Disconnected"));

      vars.states.otStatus = false;
    }

    // If boiler is disconnected, no need try setting other OT stuff
    if (!vars.states.otStatus) {
      vars.states.heating = false;
      vars.states.dhw = false;
      vars.states.flame = false;
      vars.states.fault = false;
      vars.states.diagnostic = false;

      return;
    }

    if (vars.parameters.heatingEnabled != heatingEnabled) {
      this->prevUpdateNonEssentialVars = 0;
      vars.parameters.heatingEnabled = heatingEnabled;
      Log.sinfoln(FPSTR(L_OT_HEATING), "%s", heatingEnabled ? F("Enabled") : F("Disabled"));
    }

    vars.states.heating = ot->isCentralHeatingActive(localResponse);
    vars.states.dhw = settings.opentherm.dhwPresent ? ot->isHotWaterActive(localResponse) : false;
    vars.states.flame = ot->isFlameOn(localResponse);
    vars.states.fault = ot->isFault(localResponse);
    vars.states.diagnostic = ot->isDiagnostic(localResponse);

    // These parameters will be updated every minute
    if (millis() - this->prevUpdateNonEssentialVars > 60000) {
      if (!heatingEnabled && settings.opentherm.modulationSyncWithHeating) {
        if (setMaxModulationLevel(0)) {
          Log.snoticeln(FPSTR(L_OT_HEATING), F("Set max modulation 0% (off)"));

        } else {
          Log.swarningln(FPSTR(L_OT_HEATING), F("Failed set max modulation 0% (off)"));
        }

      } else {
        if (setMaxModulationLevel(settings.heating.maxModulation)) {
          Log.snoticeln(FPSTR(L_OT_HEATING), F("Set max modulation %hhu%%"), settings.heating.maxModulation);

        } else {
          Log.swarningln(FPSTR(L_OT_HEATING), F("Failed set max modulation %hhu%%"), settings.heating.maxModulation);
        }
      }


      // Get DHW min/max temp (if necessary)
      if (settings.opentherm.dhwPresent) {
        if (updateMinMaxDhwTemp()) {
          if (settings.dhw.minTemp < vars.parameters.dhwMinTemp) {
            settings.dhw.minTemp = vars.parameters.dhwMinTemp;
            fsSettings.update();
            Log.snoticeln(FPSTR(L_OT_DHW), F("Updated min temp: %hhu"), settings.dhw.minTemp);
          }

          if (settings.dhw.maxTemp > vars.parameters.dhwMaxTemp) {
            settings.dhw.maxTemp = vars.parameters.dhwMaxTemp;
            fsSettings.update();
            Log.snoticeln(FPSTR(L_OT_DHW), F("Updated max temp: %hhu"), settings.dhw.maxTemp);
          }

        } else {
          Log.swarningln(FPSTR(L_OT_DHW), F("Failed get min/max temp"));
        }

        if (settings.dhw.minTemp >= settings.dhw.maxTemp) {
          settings.dhw.minTemp = 30;
          settings.dhw.maxTemp = 60;
          fsSettings.update();
        }
      }


      // Get heating min/max temp
      if (updateMinMaxHeatingTemp()) {
        if (settings.heating.minTemp < vars.parameters.heatingMinTemp) {
          settings.heating.minTemp = vars.parameters.heatingMinTemp;
          fsSettings.update();
          Log.snoticeln(FPSTR(L_OT_HEATING), F("Updated min temp: %hhu"), settings.heating.minTemp);
        }

        if (settings.heating.maxTemp > vars.parameters.heatingMaxTemp) {
          settings.heating.maxTemp = vars.parameters.heatingMaxTemp;
          fsSettings.update();
          Log.snoticeln(FPSTR(L_OT_HEATING), F("Updated max temp: %hhu"), settings.heating.maxTemp);
        }

      } else {
        Log.swarningln(FPSTR(L_OT_HEATING), F("Failed get min/max temp"));
      }

      if (settings.heating.minTemp >= settings.heating.maxTemp) {
        settings.heating.minTemp = 20;
        settings.heating.maxTemp = 90;
        fsSettings.update();
      }

      // Force set max heating temp
      setMaxHeatingTemp(settings.heating.maxTemp);

      // Get outdoor temp (if necessary)
      if (settings.sensors.outdoor.type == 0) {
        updateOutsideTemp();
      }

      // Get fault code (if necessary)
      if (vars.states.fault) {
        updateFaultCode();
      }

      updatePressure();

      this->prevUpdateNonEssentialVars = millis();
    }


    // Get current modulation level (if necessary)
    if ((settings.opentherm.dhwPresent && settings.dhw.enable) || settings.heating.enable || heatingEnabled) {
      updateModulationLevel();

    } else {
      vars.sensors.modulation = 0;
    }

    // Update DHW sensors (if necessary)
    if (settings.opentherm.dhwPresent) {
      updateDhwTemp();
      updateDhwFlowRate();

    } else {
      vars.temperatures.dhw = 0.0f;
      vars.sensors.dhwFlowRate = 0.0f;
    }

    // Get current heating temp
    updateHeatingTemp();


    // Fault reset action
    if (vars.actions.resetFault) {
      if (vars.states.fault) {
        if (ot->sendBoilerReset()) {
          Log.sinfoln(FPSTR(L_OT), F("Boiler fault reset successfully"));

        } else {
          Log.serrorln(FPSTR(L_OT), F("Boiler fault reset failed"));
        }
      }

      vars.actions.resetFault = false;
    }

    // Diag reset action
    if (vars.actions.resetDiagnostic) {
      if (vars.states.diagnostic) {
        if (ot->sendServiceReset()) {
          Log.sinfoln(FPSTR(L_OT), F("Boiler diagnostic reset successfully"));
          
        } else {
          Log.serrorln(FPSTR(L_OT), F("Boiler diagnostic reset failed"));
        }
      }

      vars.actions.resetDiagnostic = false;
    }


    // Update DHW temp
    byte newDhwTemp = settings.dhw.target;
    if (settings.opentherm.dhwPresent && settings.dhw.enable && (needSetDhwTemp() || newDhwTemp != currentDhwTemp)) {
      if (newDhwTemp < settings.dhw.minTemp || newDhwTemp > settings.dhw.maxTemp) {
        newDhwTemp = constrain(newDhwTemp, settings.dhw.minTemp, settings.dhw.maxTemp);
      }

      Log.sinfoln(FPSTR(L_OT_DHW), F("Set temp = %u"), newDhwTemp);

      // Set DHW temp
      if (ot->setDhwTemp(newDhwTemp)) {
        currentDhwTemp = newDhwTemp;
        this->dhwSetTempTime = millis();

      } else {
        Log.swarningln(FPSTR(L_OT_DHW), F("Failed set temp"));
      }

      // Set DHW temp to CH2
      if (settings.opentherm.dhwToCh2) {
        if (!ot->setHeatingCh2Temp(newDhwTemp)) {
          Log.swarningln(FPSTR(L_OT_DHW), F("Failed set ch2 temp"));
        }
      }
    }


    // Update heating temp
    if (heatingEnabled && (needSetHeatingTemp() || fabs(vars.parameters.heatingSetpoint - currentHeatingTemp) > 0.0001)) {
      Log.sinfoln(FPSTR(L_OT_HEATING), F("Set temp = %u"), vars.parameters.heatingSetpoint);

      // Set heating temp
      if (ot->setHeatingCh1Temp(vars.parameters.heatingSetpoint)) {
        currentHeatingTemp = vars.parameters.heatingSetpoint;
        this->heatingSetTempTime = millis();

      } else {
        Log.swarningln(FPSTR(L_OT_HEATING), F("Failed set temp"));
      }

      // Set heating temp to CH2
      if (settings.opentherm.heatingCh1ToCh2) {
        if (!ot->setHeatingCh2Temp(vars.parameters.heatingSetpoint)) {
          Log.swarningln(FPSTR(L_OT_HEATING), F("Failed set ch2 temp"));
        }
      }
    }


    // Hysteresis
    // Only if enabled PID or/and Equitherm
    if (settings.heating.hysteresis > 0 && (!vars.states.emergency || settings.emergency.usePid) && (settings.equitherm.enable || settings.pid.enable)) {
      float halfHyst = settings.heating.hysteresis / 2;
      if (this->pump && vars.temperatures.indoor - settings.heating.target + 0.0001 >= halfHyst) {
        this->pump = false;

      } else if (!this->pump && vars.temperatures.indoor - settings.heating.target - 0.0001 <= -(halfHyst)) {
        this->pump = true;
      }

    } else if (!this->pump) {
      this->pump = true;
    }
  }

  bool isReady() {
    return millis() - this->startupTime > this->readyTime;
  }

  bool needSetDhwTemp() {
    return millis() - this->dhwSetTempTime > this->dhwSetTempInterval;
  }

  bool needSetHeatingTemp() {
    return millis() - this->heatingSetTempTime > this->heatingSetTempInterval;
  }

  bool updateSlaveConfig() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::SConfigSMemberIDcode,
      0
    ));
    
    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.parameters.slaveMemberId = response & 0xFF;
    vars.parameters.slaveFlags = (response & 0xFFFF) >> 8;

    /*uint8_t flags = (response & 0xFFFF) >> 8;
    Log.straceln(
      "OT",
      F("MasterMemberIdCode:\r\n  DHW present: %u\r\n  Control type: %u\r\n  Cooling configuration: %u\r\n  DHW configuration: %u\r\n  Pump control: %u\r\n  CH2 present: %u\r\n  Remote water filling function: %u\r\n  Heat/cool mode control: %u\r\n  Slave MemberID Code: %u\r\n  Raw: %u"),
      (bool) (flags & 0x01),
      (bool) (flags & 0x02),
      (bool) (flags & 0x04),
      (bool) (flags & 0x08),
      (bool) (flags & 0x10),
      (bool) (flags & 0x20),
      (bool) (flags & 0x40),
      (bool) (flags & 0x80),
      response & 0xFF,
      response
    );*/

    return true;
  }

  /**
   * @brief Set the Master Config
   * From slave member id code:
   * id: slave.memberIdCode & 0xFF,
   * flags: (slave.memberIdCode & 0xFFFF) >> 8
   * @param id 
   * @param flags 
   * @param force 
   * @return true 
   * @return false 
   */
  bool setMasterConfig(uint8_t id, uint8_t flags, bool force = false) {
    //uint8_t configId = settings.opentherm.memberIdCode & 0xFF;
    //uint8_t configFlags = (settings.opentherm.memberIdCode & 0xFFFF) >> 8;

    vars.parameters.masterMemberId = (force || id || settings.opentherm.memberIdCode > 65535) 
      ? id 
      : vars.parameters.slaveMemberId;

    vars.parameters.masterFlags = (force || flags || settings.opentherm.memberIdCode > 65535)
      ? flags
      : vars.parameters.slaveFlags;

    unsigned int request = (unsigned int) vars.parameters.masterMemberId | (unsigned int) vars.parameters.masterFlags << 8;
    // if empty request
    if (!request) {
      return true;
    }

    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::WRITE_DATA,
      OpenThermMessageID::MConfigMMemberIDcode,
      request
    ));

    return ot->isValidResponse(response);
  }

  bool setMaxModulationLevel(byte value) {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::WRITE_DATA,
      OpenThermMessageID::MaxRelModLevelSetting,
      ot->toF88(value)
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.parameters.maxModulation = ot->fromF88(response);
    return true;
  }

  bool updateSlaveOtVersion() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::OpenThermVersionSlave,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.parameters.slaveOtVersion = ot->getFloat(response);
    return true;
  }

  bool setMasterOtVersion(float version) {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::WRITE_DATA,
      OpenThermMessageID::OpenThermVersionMaster,
      ot->toF88(version)
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.parameters.masterOtVersion = ot->fromF88(response);

    return true;
  }

  bool updateSlaveVersion() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::SlaveVersion,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.parameters.slaveVersion = response & 0xFF;
    vars.parameters.slaveType = (response & 0xFFFF) >> 8;

    return true;
  }

  bool setMasterVersion(uint8_t version, uint8_t type) {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::WRITE_DATA,
      OpenThermMessageID::MasterVersion,
      (unsigned int) version | (unsigned int) type << 8
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.parameters.masterVersion = response & 0xFF;
    vars.parameters.masterType = (response & 0xFFFF) >> 8;

    return true;
  }

  bool updateMinMaxDhwTemp() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::TdhwSetUBTdhwSetLB,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    byte minTemp = response & 0xFF;
    byte maxTemp = (response & 0xFFFF) >> 8;

    if (minTemp >= 0 && maxTemp > 0 && maxTemp > minTemp) {
      vars.parameters.dhwMinTemp = minTemp;
      vars.parameters.dhwMaxTemp = maxTemp;

      return true;
    }

    return false;
  }

  bool updateMinMaxHeatingTemp() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::MaxTSetUBMaxTSetLB,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    byte minTemp = response & 0xFF;
    byte maxTemp = (response & 0xFFFF) >> 8;

    if (minTemp >= 0 && maxTemp > 0 && maxTemp > minTemp) {
      vars.parameters.heatingMinTemp = minTemp;
      vars.parameters.heatingMaxTemp = maxTemp;
      return true;
    }

    return false;
  }

  bool setMaxHeatingTemp(byte value) {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermMessageType::WRITE_DATA,
      OpenThermMessageID::MaxTSet,
      ot->temperatureToData(value)
    ));

    return ot->isValidResponse(response);
  }

  bool updateOutsideTemp() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::Toutside,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.temperatures.outdoor = ot->getFloat(response) + settings.sensors.outdoor.offset;
    return true;
  }

  bool updateHeatingTemp() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermMessageType::READ_DATA,
      OpenThermMessageID::Tboiler,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    float value = ot->getFloat(response);
    if (value <= 0) {
      return false;
    }

    vars.temperatures.heating = value;
    return true;
  }


  bool updateDhwTemp() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermMessageType::READ_DATA,
      OpenThermMessageID::Tdhw,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    float value = ot->getFloat(response);
    if (value <= 0) {
      return false;
    }

    vars.temperatures.dhw = value;
    return true;
  }

  bool updateDhwFlowRate() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermMessageType::READ_DATA,
      OpenThermMessageID::DHWFlowRate,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }
    
    float value = ot->getFloat(response);
    if (value > 16 && this->dhwFlowRateMultiplier != 10) {
      this->dhwFlowRateMultiplier = 10;
    }
    vars.sensors.dhwFlowRate = this->dhwFlowRateMultiplier == 1 ? value : value / this->dhwFlowRateMultiplier;
    
    return true;
  }

  bool updateFaultCode() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::ASFflags,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    vars.sensors.faultCode = response & 0xFF;
    return true;
  }

  bool updateModulationLevel() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::RelModLevel,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    float modulation = ot->fromF88(response);
    if (!vars.states.flame) {
      vars.sensors.modulation = 0;
    } else {
      vars.sensors.modulation = modulation;
    }

    return true;
  }

  bool updatePressure() {
    unsigned long response = ot->sendRequest(ot->buildRequest(
      OpenThermRequestType::READ_DATA,
      OpenThermMessageID::CHPressure,
      0
    ));

    if (!ot->isValidResponse(response)) {
      return false;
    }

    float value = ot->getFloat(response);
    if (value > 5 && this->pressureMultiplier != 10) {
      this->pressureMultiplier = 10;
    }
    vars.sensors.pressure = this->pressureMultiplier == 1 ? value : value / this->pressureMultiplier;

    return true;
  }
};

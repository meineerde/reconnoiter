package com.omniti.reconnoiter;

import java.lang.System;
import com.omniti.reconnoiter.AMQListener;
import com.espertech.esper.client.*;

import org.apache.log4j.BasicConfigurator;

class IEPEngine {
  static public void main(String[] args) {
    BasicConfigurator.configure();

    Configuration config = new Configuration();
    config.addEventTypeAutoName("com.omniti.reconnoiter.event");
    EPServiceProvider epService = EPServiceProviderManager.getDefaultProvider(config);

    AMQListener l = new AMQListener(epService);

    l.run();
  }
}

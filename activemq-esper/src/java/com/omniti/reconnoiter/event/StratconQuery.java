package com.omniti.reconnoiter.event;

import java.lang.System;

import com.omniti.reconnoiter.StratconMessage;
import com.espertech.esper.client.EPStatement;
import com.espertech.esper.client.UpdateListener;
import java.util.UUID;
import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.Node;

public class StratconQuery extends StratconMessage {
  private EPStatement statement;
  private UpdateListener listener;
  private UUID uuid;
  private String name;
  private String expression;

  public StratconQuery(Document d) {
    this.uuid = UUID.randomUUID();

    Element e = d.getDocumentElement();
    name = e.getAttribute("name");
    if(name == null) name = "default";
    expression = e.getTextContent();
  }
  public UUID getUUID() {
    return uuid;
  }
  public String getExpression() {
    return expression;
  }
  public String getName() {
    return name;
  }
  public void setStatement(EPStatement s) {
    this.statement = s;
  }
  public void setListener(UpdateListener l) {
    this.listener = l;
  }
  public void destroy() {
    statement.removeListener(listener);
    statement.destroy();
  }
}

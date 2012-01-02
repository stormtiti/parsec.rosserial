/*
 * Copyright (C) 2011 Google Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

package org.ros.rosserial;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.ros.RosCore;
import org.ros.internal.node.DefaultNodeFactory;
import org.ros.internal.node.NodeFactory;
import org.ros.message.MessageListener;
import org.ros.message.rosserial_msgs.TopicInfo;
import org.ros.node.DefaultNodeRunner;
import org.ros.node.Node;
import org.ros.node.NodeConfiguration;
import org.ros.node.NodeRunner;
import org.ros.node.topic.CountDownSubscriberListener;
import org.ros.node.topic.Subscriber;

import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.PipedInputStream;
import java.io.PipedOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * @author damonkohler@google.com (Damon Kohler)
 */
public class IntegrationTest {

  @Test
  public void testSubscribeToSerialPublisher() throws IOException, InterruptedException {
    RosCore core = RosCore.newPrivate();
    core.start();
    core.awaitStart();

    NodeRunner nodeRunner = DefaultNodeRunner.newDefault();
    NodeFactory nodeFactory = new DefaultNodeFactory();
    NodeConfiguration nodeConfiguration = NodeConfiguration.newPrivate(core.getUri());
    nodeConfiguration.setNodeName("node");
    Node node = nodeFactory.newNode(nodeConfiguration);

    CountDownSubscriberListener subscriberListener = new CountDownSubscriberListener();
    final CountDownLatch latch = new CountDownLatch(1);
    Subscriber<org.ros.message.std_msgs.String> subscriber =
        node.newSubscriber("hello_world", "std_msgs/String");
    subscriber.addMessageListener(new MessageListener<org.ros.message.std_msgs.String>() {
      @Override
      public void onNewMessage(org.ros.message.std_msgs.String message) {
        assertEquals("Hello, world!", message.data);
        latch.countDown();
      }
    });
    subscriber.addSubscriberListener(subscriberListener);
    assertTrue(subscriberListener.awaitMasterRegistrationSuccess(5, TimeUnit.SECONDS));

    // Create client (e.g. Arduino) and host (e.g. tablet) streams.
    PipedInputStream clientInputStream = new PipedInputStream();
    PipedOutputStream hostOutputStream = new PipedOutputStream();
    clientInputStream.connect(hostOutputStream);

    PipedInputStream hostInputStream = new PipedInputStream();
    PipedOutputStream clientOutputStream = new PipedOutputStream();
    hostInputStream.connect(clientOutputStream);

    RosSerial rosSerial = new RosSerial(new BufferedInputStream(hostInputStream), hostOutputStream);
    nodeConfiguration.setNodeName("rosserial");
    nodeRunner.run(rosSerial, nodeConfiguration);

    // Topic negotiation request.
    byte[] expectedTopicNegotiationBuffer =
        new byte[] { (byte) 0xFF, (byte) 0xFF, 0, 0, 0, 0, (byte) 0xFF };
    byte[] topicNegotiationBuffer = new byte[expectedTopicNegotiationBuffer.length];
    clientInputStream.read(topicNegotiationBuffer);
    assertArrayEquals(expectedTopicNegotiationBuffer, topicNegotiationBuffer);

    final PacketSender packetSender = new DefaultPacketSender(clientOutputStream, node.getLog());

    // Topic negotiation response.
    TopicInfo topicInfo = new TopicInfo();
    topicInfo.message_type = "std_msgs/String";
    topicInfo.topic_id = 101;
    topicInfo.topic_name = "hello_world";
    byte[] data = Protocol.constructMessage(0, topicInfo);
    packetSender.send(data);

    // Publish hello world over serial continuously.
    Thread publisherThread = new Thread() {
      @Override
      public void run() {
        while (!Thread.currentThread().isInterrupted()) {
          org.ros.message.std_msgs.String helloWorld = new org.ros.message.std_msgs.String();
          helloWorld.data = "Hello, world!";
          byte[] data = Protocol.constructMessage(101, helloWorld);
          packetSender.send(data);
          try {
            Thread.sleep(500);
          } catch (InterruptedException e) {
            // Cancelable
          }
        }
      };
    };
    publisherThread.start();

    assertTrue(latch.await(5, TimeUnit.SECONDS));

    publisherThread.interrupt();
    nodeRunner.shutdownNodeMain(rosSerial);
    node.shutdown();
  }
}

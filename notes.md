# ROS3 ("Rose") deveopment notes


Butler Lampson "Hints and Principles for Computer System Design"

We want ROS3 to be STEADY through AID
* Simple - Paramount - "The price of reliability is the pursuit of upmost simplicity" - Hoare
  * Is is simple enough - can I understand it, will I still understand it when it changes?
  * keep it simple
  * write interfaces to abstractions
  * Write a spec - highl level description
* Timely (to market) - I only have so much time to wait
* Efficient - for the implementer, for the robot. Very important
  * "An efficient program is an excercise in logical brinkmanship." - Djikstra
  * Measure first, then napkin-math modeling, then:
    * Cache - ram cache, file/database buffers
    * Batch - group commit, pipes, epochs
    * Precompute - web search, vector stores, database indexes
    * Reorder - copy on right, eventual consistency, optimistic concurrecny control
    * Better algorithms, parallelize, approximate
* Adaptable - plan for future success. Very important
* Dependable - reliable, available, secure, undo-able (easy shutdown); Extremely important
* Yummy - like apple, not micrsoft. Extremely important important since I use these tools.

* Approximate - vs precise, we need some combination.
  * Good enough - web, search engines, ip packets
    * eventual consistency - DNS, Dynamo, file/email sync
  * Loose coupling - springy flaky parts
  * Brute force - overprevision, broadcast, scan, crash fast
    * Strengthen (do more than needed): redo log, coarse locks
  * Relax - small steps converge (exponential backoff, daily builds)
  * Hints - trust, but verify
* Incrimental
  * Indirection
  * Iteration - redo (log), undo (snapshots), scale (internet, clusters, I/O)
  * Extend - HTML, Ethernet - extension over replacement
* Divide and conquer
  * interfaces/layers divided for abstraction
  * structure - divide by structure for efficient movement
  * redundancy - divide in time (retransmission) or space (replicated state machines)
  * performance (stripe, stream, or struggle)

* Get it right!

# see
- https://wiki.ros.org/ROS/Tutorials/WritingPublisherSubscriber%28c%2B%2B%29

for c example of pub/sub. 

replicate..

using quicksand and mirage (minimal modifications.)

fun names-

0.x: Zephyranthes - rain lily
1.0: adenium - desert rose


### ROS1 Publisher
```
#include "ros/ros.h"
#include "std_msgs/String.h"

#include <sstream>

/**
 * This tutorial demonstrates simple sending of messages over the ROS system.
 */
int main(int argc, char **argv)
{
  /**
   * The ros::init() function needs to see argc and argv so that it can perform
   * any ROS arguments and name remapping that were provided at the command line.
   * For programmatic remappings you can use a different version of init() which takes
   * remappings directly, but for most command-line programs, passing argc and argv is
   * the easiest way to do it.  The third argument to init() is the name of the node.
   *
   * You must call one of the versions of ros::init() before using any other
   * part of the ROS system.
   */
  ros::init(argc, argv, "talker");

  /**
   * NodeHandle is the main access point to communications with the ROS system.
   * The first NodeHandle constructed will fully initialize this node, and the last
   * NodeHandle destructed will close down the node.
   */
  ros::NodeHandle n;

  /**
   * The advertise() function is how you tell ROS that you want to
   * publish on a given topic name. This invokes a call to the ROS
   * master node, which keeps a registry of who is publishing and who
   * is subscribing. After this advertise() call is made, the master
   * node will notify anyone who is trying to subscribe to this topic name,
   * and they will in turn negotiate a peer-to-peer connection with this
   * node.  advertise() returns a Publisher object which allows you to
   * publish messages on that topic through a call to publish().  Once
   * all copies of the returned Publisher object are destroyed, the topic
   * will be automatically unadvertised.
   *
   * The second parameter to advertise() is the size of the message queue
   * used for publishing messages.  If messages are published more quickly
   * than we can send them, the number here specifies how many messages to
   * buffer up before throwing some away.
   */
  ros::Publisher chatter_pub = n.advertise<std_msgs::String>("chatter", 1000);

  ros::Rate loop_rate(10);

  /**
   * A count of how many messages we have sent. This is used to create
   * a unique string for each message.
   */
  int count = 0;
  while (ros::ok())
  {
    /**
     * This is a message object. You stuff it with data, and then publish it.
     */
    std_msgs::String msg;

    std::stringstream ss;
    ss << "hello world " << count;
    msg.data = ss.str();

    ROS_INFO("%s", msg.data.c_str());

    /**
     * The publish() function is how you send messages. The parameter
     * is the message object. The type of this object must agree with the type
     * given as a template parameter to the advertise<>() call, as was done
     * in the constructor above.
     */
    chatter_pub.publish(msg);

    ros::spinOnce();

    loop_rate.sleep();
    ++count;
  }


  return 0;
}
```

### ROS1 Subscriber
```
#include "ros/ros.h"
#include "std_msgs/String.h"

/**
 * This tutorial demonstrates simple receipt of messages over the ROS system.
 */
void chatterCallback(const std_msgs::String::ConstPtr& msg)
{
  ROS_INFO("I heard: [%s]", msg->data.c_str());
}

int main(int argc, char **argv)
{
  /**
   * The ros::init() function needs to see argc and argv so that it can perform
   * any ROS arguments and name remapping that were provided at the command line.
   * For programmatic remappings you can use a different version of init() which takes
   * remappings directly, but for most command-line programs, passing argc and argv is
   * the easiest way to do it.  The third argument to init() is the name of the node.
   *
   * You must call one of the versions of ros::init() before using any other
   * part of the ROS system.
   */
  ros::init(argc, argv, "listener");

  /**
   * NodeHandle is the main access point to communications with the ROS system.
   * The first NodeHandle constructed will fully initialize this node, and the last
   * NodeHandle destructed will close down the node.
   */
  ros::NodeHandle n;

  /**
   * The subscribe() call is how you tell ROS that you want to receive messages
   * on a given topic.  This invokes a call to the ROS
   * master node, which keeps a registry of who is publishing and who
   * is subscribing.  Messages are passed to a callback function, here
   * called chatterCallback.  subscribe() returns a Subscriber object that you
   * must hold on to until you want to unsubscribe.  When all copies of the Subscriber
   * object go out of scope, this callback will automatically be unsubscribed from
   * this topic.
   *
   * The second parameter to the subscribe() function is the size of the message
   * queue.  If messages are arriving faster than they are being processed, this
   * is the number of messages that will be buffered up before beginning to throw
   * away the oldest ones.
   */
  ros::Subscriber sub = n.subscribe("chatter", 1000, chatterCallback);

  /**
   * ros::spin() will enter a loop, pumping callbacks.  With this version, all
   * callbacks will be called from within this thread (the main one).  ros::spin()
   * will exit when Ctrl-C is pressed, or the node is shutdown by the master.
   */
  ros::spin();

  return 0;
}
```

### ROS2
we do not talk about the nightmare of garbage API design that is ROS2.


## Potential New (ROS3) API (C-safe):
rose_make_sub

rose_sub_create
rose_pub_create
rose_node_create

rose_create_pub

#define ROSE_MEM_FREE free
#define ROSE_MEM_ALLOC malloc

    rose_nodehandle n = rose_init(argc, argv, "name")

    rose_publisher p = rose_make_pub(n, "topic", 100, 1000);

    rose_subscriber s = rose_make_sub(n, "topic", callback, 0); // queue or not

    rose_get_param(n, "paramname")

    rose_set_param(n, "/paramname")  // <- global
    rose_set_param(n, "paramname")  // <- default local

    rose_publish(p, message)

    rose_spin_once()
    rose_spin_forever()  # todo await changes on atomic variable?>

    # manual sub:
    if(rose_subscriber_remaining(s)) {
      callback(rose_subscriber_read(s))
      callback(rose_subscriber_readlast(s))
    }


    rose_sleep() // double seconds
    rose_rate_sleep(10) // static var stores prev call time, simple 1-hist prediction

rosemsg_write_start_  <- inject pid
rosemsg_read_start_ 
rosesmg_read_fullstart_ <- read pid

make static node on the first rose_init.

rose_info(n, str) // prints and logs to global messages topic
rose_warn(n, str)
rose_error(n, str)
rose_fatal(n, str)


### User Request

Please consider this new ROS3 "Rose" API. Analyze the functions and suggest any modifications to make it easier to use as a program for creating large-scale distributed systems out of small ros nodes in different languages.
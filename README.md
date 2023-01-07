# Final Submission Work
All work are done by the sole contributor of tgroup88: Bert Yan

A channel is a model for synchronization via message passing. Messages may be sent over a channel by a
thread (Sender) and other threads which have a reference to this channel can receive them (Receivers).
A channel can have multiple senders and receivers referencing it at any point of time.
Channels are used as a primitive to implement various other concurrent programming constructs. For
example, channels are heavily used in Google’s Go programming language and are very useful
frameworks for high-level concurrent programming. In this lab you will be writing your own version of a
channel which will be used to communicate among multiple clients. A client can either write onto the
channel or read from it. Keep in mind that multiple clients can read and write simultaneously from the
channel. You are encouraged to explore the design space creatively and implement a channel that is
correct and not exceptionally slow or inefficient. Performance is not the main concern in this
assignment (functionality is the main concern), but your implementation should avoid inefficient designs
that sleep for any fixed time or unnecessarily waste CPU time.

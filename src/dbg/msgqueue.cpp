#include "msgqueue.h"

// Allocate a message stack
MESSAGE_STACK* MsgAllocStack()
{
    auto stack = new MESSAGE_STACK;

    stack->WaitingCalls = 0;
    stack->Destroy = false;

    return stack;
}

// Free a message stack
void MsgFreeStack(MESSAGE_STACK* Stack)
{
    ASSERT_NONNULL(Stack);

    // Update termination variable
    Stack->Destroy = true;

    // Notify each thread
    for(int i = 0; i < Stack->WaitingCalls + 1; i++)
    {
        MESSAGE newMessage;
        Stack->msgs.enqueue(newMessage);
    }

    // Delete allocate structure
    delete Stack;
}

// Add a message to the stack
bool MsgSend(MESSAGE_STACK* Stack, int Msg, duint Param1, duint Param2)
{
    if(Stack->Destroy)
        return false;

    MESSAGE newMessage;
    newMessage.msg = Msg;
    newMessage.param1 = Param1;
    newMessage.param2 = Param2;

    // Asynchronous send
    asend(Stack->msgs, newMessage);
    return true;
}

// Get a message from the stack (will return false when there are no messages)
bool MsgGet(MESSAGE_STACK* Stack, MESSAGE* Msg)
{
    if(Stack->Destroy)
        return false;

    // Don't increment the wait count because this does not wait
    return try_receive(Stack->msgs, *Msg);
}

// Wait for a message on the specified stack
void MsgWait(MESSAGE_STACK* Stack, MESSAGE* Msg)
{
    if(Stack->Destroy)
        return;

    // Increment/decrement wait count
    InterlockedIncrement((volatile long*)&Stack->WaitingCalls);
    *Msg = Stack->msgs.dequeue();
    InterlockedDecrement((volatile long*)&Stack->WaitingCalls);
}

#Assignment 4 directory

This directory contains source code and other files for Assignment 4.

Use this README document to store notes about design, testing, and
questions you have while developing your assignment.

I am starting the design for this assignment using Tutor Brian Nguyen's guidelines/pseudocode.

Questions asked to ChatGPT: 
I have a queue_pop function that sets the value of its void **elem arg to the element that it pots out. How should I pass in my uintptr_t variable to the function when its header is the following: bool queue_pop(queue_t *q, void **elem);

How can I initialize the uintrptr_t variable to NULL?

Design Idea based on TA Vincent Slides:
Create a global linked map where I can check within each GET/PUT request if the uri has a lock already,
and if it does: check its priority, if not: create one and add a key/value Node to the list to document.

In this assignment, I used TA Mitchell's pseudo from his section slides. I committed frequently with 
debugging prints showing my progress as well as using ChatGPT/Github Copilot for most of the struct functions
(constructor, destructor, searching HT, getting index (hash function)).

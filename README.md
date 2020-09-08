# kernel-chess

In the second part of this project, I implemented a kernel module to allow a user to play chess with the computer. I implemented it as a character device driver. That is, the program creates a virtual device (or devices) which can be accessed through a file descriptor (i.e. /dev/chess, or /dev/chess-%d if we are allowing multiple devices to be in existence at the same time).
The basic functionality includes:
- __init and __exit functions which properly initialize the device(s) and clean up after them (by unregistering them);
- file_operations data structure, which maps the open, release, read, and write functions
- open and release are trivial
- the read function outputs the most recent message from the module (message is stored in the global d_data data structure which maintains the necessary information about each device)
- the write function accepts user input, checks it for validity, parses it and acts accordingly, thus allowing the user to play the game
- for an in-depth description of how the computer moves are generated, player moves validated, and for how I check for check and checkmate, please refer to the design document.
- locking is provided through the use of mutexes
- the data associated with each device is stored in the d_data structure and includes the cdev structure and the appropriate information about the game (whether a game is in progress, the state of the game board (including the board array and the figures array), whose turn it is, player's and computer's tokens, and the most recent message).

Additional functionality (extra credit):
- provide support for multiple games at once. ***You may want to change the maximum number of devices allowed. You can do so by changing the value of the MAX_MINOR constant. The file descriptors have the following format: /dev/chess-%d, where %d is the device's minor number. By default, the constant is set to 1. Please refer to the design document for more information.

References (outside of those provided by Prof. Sebald):
I used the following tutorial to get started with the module (merely as the first step):
olegkutkov.me/2018/03/14/simple-linux-character-device-driver/

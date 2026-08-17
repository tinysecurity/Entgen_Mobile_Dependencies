# Entgen_Mobile_Dependencies
Scripts, tools and prebuilt services to set up Entgen Mobile

## HOW TO INSTALL
Disclaimer: I am just 1 dev. These commands are were designed with a Debian-based Linux environment in mind, specifically Raspian or RaspiOS. These commands will work on a headless environment with just a shell. If your environment differs and you encounter difficulties, please open a Github issue and thoroughly describe your issue.

- You need: gh, git, ssh enabled, wifi
- Copy the HTTPS URL from the repo under the green Code button.
- In your terminal: `git clone https://github.com/tinysecurity/Entgen_Mobile_Dependencies.git`
- Change directory using `cd Entgen_Mobile_Dependencies` and `ls` to list all files.
- You should see the install.sh in the list. To make it runnable, run `chmod +x install.sh`. This gives the file execution permissions.
- To run it, type `source install.sh` and hit enter.
- The installer should take care of the majority of the installation, but you may be asked to enter your password when it executes an action as the super user or approve a prompt. 
- When finished, a QR code should pop up on the screen.
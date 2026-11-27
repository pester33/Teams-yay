# v1.3.6
- iOS support

# v1.3.5
- Pause menu: Team, Playtest, Copy to My Levels and Goals are now a row at the bottom
- Fixed Save and Play not being renamed to Playtest in team levels

# v1.3.4
- Cursors and edit flashes no longer live inside GD's object layer (safer with other editor mods like Editor Tab API)

# v1.3.3
- Fixed old copies of objects staying behind when someone edits them off screen (typing text left h, he, hel, hello)

# v1.3.2
- Test and Play is now Playtest: it saves the team level online and starts the playtest right in the editor (no more leaving the editor and loading forever)

# v1.3.1
- Fixed a crash with Editor Tab API when pausing: the pause menu is no longer changed, Save and Play is just renamed to Test and Play in team levels

# v1.3.0
- Test and Play: saves the team level, leaves the editor and plays it (replaces Save and Play in team levels)
- Copy to My Levels: saves your own normal copy of the team level (not synced)
- Both are in the Team menu; Test and Play also takes Save and Play's spot in the pause menu

# v1.2.2
- Fixed Save and Play loading forever on team levels: team levels now open through the level page like your own levels

# v1.2.1
- Leaving the editor of a team level no longer does its saving in the middle of GD switching screens (trying to fix the endless loading screen)

# v1.2.0
- Update log: press Update log in Team Gaming to see every version's changes
- After an update, a What's new popup shows what changed
- The update popup shows what's in the new version before you install it

# v1.1.2
- Delete button in the Team Gaming list for levels you're admin of

# v1.1.1
- PC: your cursor now shows where you last placed or moved a block (mouse tracking didn't work on PC)

# v1.1.0
- Admin panel for whoever made the team level: rename it, set a join code, kick, ban / unban, delete
- Team goals: write what you want to do on a level, tick them off, see who did what
- Goals progress and a lock for levels with a join code in the Team Gaming list
- Needs sql/admin.sql run once on the database

# v1.0.0
- Team Gaming button on the main menu with the list of team levels
- Open a team level and build it together live (cursors, names, edit flashes, colors, groups)
- Progress is saved online automatically, and when you leave the editor
- Team member list in data/members.txt

/*  CON_consolecommands.c
 *  Written By: Garrett Banuk <mongoose@mongeese.org>
 *  This is free, just be sure to give me credit when using it
 *  in any of your programs.
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "SDL.h"
#include "CON_console.hh"
#include "CON_consolecommands.hh"
#include "CON_internal.hh"


/* Linked list of all the commands. */
static CommandInfo	*Commands = NULL;


/* executes the command passed in from the string */
void CON_CommandExecute(ConsoleInformation *console)
{
	char		Command[CON_CHARS_PER_LINE];
	char		*BackStrings = console->ConsoleLines[0];
	CommandInfo	*CurrentCommand = Commands;

	
	/* Get the command out of the string */
	if(EOF == sscanf(BackStrings, "%s", Command))
		return;

	CON_NewLineConsole(console);
	
	CurrentCommand = Commands;
	while(CurrentCommand)
	{
		if(0 == strcmp(Command, CurrentCommand->CommandWord))
		{
				CurrentCommand->tocall->ConsoleCallback(BackStrings);
		}
		CurrentCommand = CurrentCommand->NextCommand;
	}
	CON_Out(console, "Bad Command");
}

/* Use this to add commands.
 * Pass in a pointer to a funtion that will take a string which contains the 
 * arguments passed to the command. Second parameter is the command to execute
 * on.
 */
void CON_AddCommand(ConsoleInterface *callableObject, const char *CommandWord)
{
	CommandInfo	**CurrentCommand = &Commands;


	while(*CurrentCommand)
		CurrentCommand = &((*CurrentCommand)->NextCommand);
	
	/* Add a command to the list */
	*CurrentCommand = (CommandInfo*)malloc(sizeof(CommandInfo));
	(*CurrentCommand)->tocall = callableObject;
	(*CurrentCommand)->CommandWord = (char*)malloc(strlen(CommandWord)+1);
	strcpy((*CurrentCommand)->CommandWord, CommandWord);
	(*CurrentCommand)->NextCommand = NULL;	
}

/* tab completes commands
 * It takes a string to complete and a pointer to the strings length. The strings
 * length will be modified if the command is completed. */
void CON_TabCompletion(ConsoleInformation *console)
{
	int		Matches = 0;
	int		*location = &console->StringLocation;
	char		*CommandLine = console->ConsoleLines[0];
	CommandInfo	*CurrentCommand;


	/* Find all the commands that match */
	CurrentCommand = Commands;
	while(CurrentCommand)
	{
		if(0 == strncmp(CommandLine, CurrentCommand->CommandWord, strlen(CommandLine)))
		Matches++;
		CurrentCommand = CurrentCommand->NextCommand;
	}
	
	/* if we got one match, find it again and execute it */
	if(Matches == 1)
	{
		CurrentCommand = Commands;
		while(CurrentCommand)
		{
			if(0 == strncmp(CommandLine, CurrentCommand->CommandWord, strlen(CommandLine)))
			{
				strcpy(CommandLine, CurrentCommand->CommandWord);
				CommandLine[strlen(CurrentCommand->CommandWord)] = ' ';
				*location = strlen(CurrentCommand->CommandWord)+1;
				CON_UpdateConsole(console);
				break;
			}
			CurrentCommand = CurrentCommand->NextCommand;
		}
	}
	else if(Matches > 1)/* multiple matches so print them out to the user */
	{
		CON_NewLineConsole(console);
		CurrentCommand = Commands;
		while(CurrentCommand)
		{
			if(0 == strncmp(CommandLine, CurrentCommand->CommandWord, strlen(CommandLine)))
				CON_Out(console, CurrentCommand->CommandWord);

			CurrentCommand = CurrentCommand->NextCommand;
		}
	}
	else if(Matches == 0)
		CON_Out(console, "Bad Command");
}


/* Lists all the commands to be used in the console */
void CON_ListCommands(ConsoleInformation *console)
{
	CommandInfo	*CurrentCommand = Commands;
	
	CON_Out(console, " ");
	while(CurrentCommand)
	{
		CON_Out(console, "%s", CurrentCommand->CommandWord);
		CurrentCommand = CurrentCommand->NextCommand;
	}
	
}


/* This frees all the memory used by the commands */
void CON_DestroyCommands()
{
	CommandInfo	*CurrentCommand = Commands;
	CommandInfo	*temp;

	while(CurrentCommand)
	{
		temp = CurrentCommand;
		CurrentCommand = CurrentCommand->NextCommand;

		free(temp->CommandWord);
		free(temp);
	}
	Commands = NULL;
}

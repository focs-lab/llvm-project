#!/bin/bash

# --- CONFIGURATION ---

# C source file to be tested
SOURCE_FILE="simple1.c"
# The binary name will be derived automatically from the source file name (e.g., simple1)
BINARY_NAME="${SOURCE_FILE%.*}"

# Compiler to use
COMPILER="clang"
# Compiler flags to enable ThreadSanitizer and add debug symbols
COMPILER_FLAGS="-fsanitize=thread -g"

# Runtime options for ThreadSanitizer
TSAN_EXEC_OPTIONS="enable_filter=0,verbosity=1"

# Log file for the last run where a data race WAS detected
RACE_DETECTED_LOG="race_found.txt"
# Log file for the first clean run (no race detected), which stops the script
NO_RACE_DETECTED_LOG="no_race_found.txt"

# The string to search for in the output to identify a TSan race report
RACE_SEARCH_STRING="WARNING: ThreadSanitizer: data race"


# --- SCRIPT LOGIC ---

# Check if the source file exists
if [ ! -f "$SOURCE_FILE" ]; then
    echo "Error: Source file '$SOURCE_FILE' not found."
    exit 1
fi

# 1. Compilation
echo "1. Compiling '$SOURCE_FILE' with $COMPILER..."
$COMPILER $COMPILER_FLAGS "$SOURCE_FILE" -o "$BINARY_NAME"

# Check if compilation was successful
if [ $? -ne 0 ]; then
    echo "Compilation failed. Aborting script."
    exit 1
fi
echo "Compilation successful. Binary created: '$BINARY_NAME'."
echo "--------------------------------------------------"

# 2. Execution Loop (runs until a clean execution)
echo "2. Running '$BINARY_NAME' in a loop until a clean run occurs..."

# Variable to store the output of the last run that had a data race
last_race_output=""
run_count=0

while true; do
    ((run_count++))
    echo -n "Attempt #${run_count}... "

    # Execute the program with TSAN_OPTIONS and redirect stderr to stdout
    echo "TSAN_OPTIONS=$TSAN_EXEC_OPTIONS ./$BINARY_NAME"
    current_output=$(TSAN_OPTIONS="$TSAN_EXEC_OPTIONS" ./"$BINARY_NAME" 2>&1)

    # Check if the output contains the data race warning
    if echo "$current_output" | grep -q "$RACE_SEARCH_STRING"; then
        # DATA RACE FOUND: continue the loop
        echo "race detected, searching for a clean run."
        # Store the race output as the last known "bad" run
        last_race_output="$current_output"
    else
        # NO DATA RACE FOUND: break the loop
        echo "CLEAN RUN DETECTED!"
        echo "--------------------------------------------------"

        # 3. Saving results to files
        echo "3. Saving results to files..."

        # Save the output of the clean run
        echo "$current_output" > "$NO_RACE_DETECTED_LOG"
        echo "Clean run output saved to: $NO_RACE_DETECTED_LOG"

        # Save the output of the last run that HAD a race
        echo "$last_race_output" > "$RACE_DETECTED_LOG"
        echo "Last race-detected output saved to: $RACE_DETECTED_LOG"

        # Break the loop
        break
    fi
done

echo "Script finished."
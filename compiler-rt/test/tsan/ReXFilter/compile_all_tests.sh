#!/bin/bash

# --- Settings ---
# Compiler to be used
COMPILER="clang"
# Compiler flags. The -pthread flag is important for correctly linking thread libraries.
COMPILER_FLAGS="-fsanitize=thread -g -pthread"

echo "Starting compilation of all .c files in the current directory..."
mkdir -p bin
echo "Using flags: $COMPILER_FLAGS"
echo "-------------------------------------------------"

# Check if any .c files exist to avoid an error if the directory is empty
shopt -s nullglob
c_files=(*.c)
if [ ${#c_files[@]} -eq 0 ]; then
    echo "No .c files found."
    exit 0
fi
shopt -u nullglob # Revert to default behavior

# Variable to count compilation errors
error_count=0

# Loop through all files ending in .c
for SOURCE_FILE in *.c; do
    # Set the binary name by removing the .c extension
    BINARY_NAME="bin/${SOURCE_FILE%.*}"

    echo "Compiling: $SOURCE_FILE -> $BINARY_NAME"

    # Run the compilation command
    $COMPILER $COMPILER_FLAGS "$SOURCE_FILE" -o "$BINARY_NAME"

    # Check the exit code of the last command ($?)
    # 0 means success, any other value is an error.
    if [ $? -eq 0 ]; then
        echo "  [✓] Success"
    else
        echo "  [✗] ERROR: Failed to compile $SOURCE_FILE."
        ((error_count++))
    fi
    echo "" # Add a blank line for better readability
done

echo "-------------------------------------------------"
if [ $error_count -eq 0 ]; then
    echo "All files compiled successfully."
else
    echo "Compilation finished. Found $error_count error(s)."
fi
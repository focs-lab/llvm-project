#!/usr/bin/env python3

import sys
import re

def calculate_escape_categories(log_file_path):
    """
    Calculates the total number of "Number of escapes" by categories from a log file.

    Args:
        log_file_path (str): Path to the log file.

    Returns:
        dict: A dictionary where keys are "Number of escapes" categories and values are their sums.
              Returns None if the file is not found or an error occurs while reading.
    """
    category_counts = {}
    try:
        with open(log_file_path, 'r') as log_file:
            for line in log_file:
                line = line.strip()
                if "Number of escapes" in line:
                    parts = line.split("Number of escapes due to ", 1)
                    if len(parts) == 2:
                        number_part = parts[0].strip()
                        category_description = parts[1].strip()

                        number_str = number_part.split()[0] # Take the first word as the number

                        try:
                            escapes = int(number_str)
                            # Clean up category description (remove leading '-')
                            category_description = category_description.lstrip('- ').strip()

                            if category_description in category_counts:
                                category_counts[category_description] += escapes
                            else:
                                category_counts[category_description] = escapes
                        except ValueError:
                            print(f"Warning: Non-numeric value found in line starting with 'Number of escapes': {line}", file=sys.stderr)
                    else:
                        # Handle cases where it's just "Number of escapes" without "due to..." if needed.
                        # For now, we'll ignore these lines as per the prompt focusing on "due to" categories.
                        if "due to" not in line:
                            pass # Ignore lines like "Number of escapes" without "due to"
                        else:
                             print(f"Warning: Unexpected format for line starting with 'Number of escapes': {line}", file=sys.stderr)


    except FileNotFoundError:
        print(f"Error: Log file not found at '{log_file_path}'", file=sys.stderr)
        return None
    except Exception as e:
        print(f"Error reading log file: {e}", file=sys.stderr)
        return None

    return category_counts

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python count_escape_categories.py <log_file>")
        sys.exit(1)

    log_file_path = sys.argv[1]
    escape_categories = calculate_escape_categories(log_file_path)

    if escape_categories is not None:
        print("Number of escapes by category:")
        for category, count in escape_categories.items():
            print(f"{category}: {count}")
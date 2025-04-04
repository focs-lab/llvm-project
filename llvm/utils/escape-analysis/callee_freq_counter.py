import argparse
from collections import Counter

def count_string_frequency(filename):
    """
    Counts the frequency of lines starting with "Callee Function: ..." in a file.

    Args:
        filename (str): Path to the file to process.

    Returns:
        dict: A dictionary where keys are function names (without "Callee Function: "),
              and values are their frequencies.
              Returns an empty dictionary if the file is not found or does not contain
              lines starting with "Callee Function: ".
    """
    function_counts = Counter()

    try:
        with open(filename, 'r') as file:
            for line in file:
                line = line.strip()  # Remove leading/trailing spaces and newlines
                if line.startswith("Callee Function: "):
                    function_name = line[len("Callee Function: "):]  # Extract function name
                    function_counts[function_name] += 1
    except FileNotFoundError:
        print(f"Error: File '{filename}' not found.")
        return {}  # Return an empty dictionary in case of file error

    return function_counts

def main():
    parser = argparse.ArgumentParser(description="Counts the frequency of lines starting with 'Callee Function: ...' in a file.")
    parser.add_argument("filename", help="Path to the input file")
    parser.add_argument("-s", "--sort", action="store_true", help="Sort the output by frequency (descending)")

    args = parser.parse_args()

    frequencies = count_string_frequency(args.filename)

    if frequencies:  # Check that the dictionary is not empty (file processed successfully)
        if args.sort:
            sorted_frequencies = frequencies.most_common()  # Sort Counter by frequency
            print("Function frequency (sorted by descending order):")
            for function, count in sorted_frequencies:
                print(f"{function}: {count}")
        else:
            print("Function frequency:")
            for function, count in frequencies.items():
                print(f"{function}: {count}")

if __name__ == "__main__":
    main()
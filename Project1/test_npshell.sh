#!/bin/bash

# Color codes for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Function to run test
run_test() {
    local test_name="$1"
    local input_file="$2"
    local expected_file="$3"
    
    echo -e "\n=== Testing: $test_name ==="
    
    # Run the shell with input from file and capture output
    ./npshell < "$input_file" > actual_output.txt 2>&1
    
    # Compare actual output with expected output
    if diff -Z actual_output.txt "$expected_file" > /dev/null; then
        echo -e "${GREEN}[PASS]${NC} $test_name"
        return 0
    else
        echo -e "${RED}[FAIL]${NC} $test_name"
        echo "Differences:"
        diff -Z actual_output.txt "$expected_file"
        return 1
    fi
}

# Ensure npshell executable exists
if [ ! -x "./npshell" ]; then
    echo "Error: npshell executable not found"
    exit 1
fi

# Initialize test results
total_tests=0
passed_tests=0

# Define test cases
test_cases=(
    "test"
    # Add more test cases as needed
)

# Run each test case
for test in "${test_cases[@]}"; do
    ((total_tests++))
    
    if run_test "$test" "${test}_input.txt" "${test}_output.txt"; then
        ((passed_tests++))
    fi
done

# Print summary
echo -e "\n=== Test Summary ==="
echo "Total Tests: $total_tests"
echo "Passed Tests: $passed_tests"
echo "Failed Tests: $((total_tests - passed_tests))"

# Clean up
# rm -f actual_output.txt

# Exit with non-zero status if any tests failed
[ $passed_tests -eq $total_tests ]
#!/bin/bash

set -u

function usage() {
    echo "Usage: $0 -i INPUT_DIR [-o OUTPUT_DIR] TARGET..."
    echo
    echo "Run SymCC-instrumented TARGET in a loop, feeding newly generated inputs back "
    echo "into it. Initial inputs are expected in INPUT_DIR, and new inputs are "
    echo "continuously read from there. If OUTPUT_DIR is specified, a copy of the corpus "
    echo "and of each generated input is preserved there. TARGET may contain the special "
    echo "string \"@@\", which is replaced with the name of the current input file."
    echo
    echo "Note that SymCC never changes the length of the input, so be sure that the "
    echo "initial inputs cover all required input lengths."
}

while getopts "i:o:" opt; do
    case "$opt" in
        i)
            in=$OPTARG
            ;;
        o)
            out=$OPTARG
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done
shift $((OPTIND-1))
target=$@
timeout="timeout -k 5 90"

if [[ ! -v in ]]; then
    echo "Please specify the input directory!"
    usage
    exit 1
fi

# Remove target file
rm -rf "target"

# Get the start time
start_time=$(date +%s)

# Declare the mark file
mark_file="mark.txt"
echo "------------------------------------------" >> "$mark_file"
echo "START TIME:$(date -d @$start_time)" >> "$mark_file"

# Create the work environment
work_dir=$(mktemp -d)
mkdir $work_dir/{next,symcc_out}
touch $work_dir/analyzed_inputs
if [[ -v out ]]; then
    mkdir -p $out
fi

function cleanup() {
    rm -rf $work_dir
}

trap cleanup EXIT

# Copy all files in the source directory to the destination directory, renaming
# them according to their hash.
function copy_with_unique_name() {
    local source_dir="$1"
    local dest_dir="$2"

    if [ -n "$(ls -A $source_dir)" ]; then
        local f
        for f in $source_dir/*; do
            local dest="$dest_dir/$(sha256sum $f | cut -d' ' -f1)"
            cp "$f" "$dest"
        done
    fi
}

# Copy files from the source directory into the next generation.
function add_to_next_generation() {
    local source_dir="$1"
    copy_with_unique_name "$source_dir" "$work_dir/next"
}

# If an output directory is set, copy the files in the source directory there.
function maybe_export() {
    local source_dir="$1"
    if [[ -v out ]]; then
        copy_with_unique_name "$source_dir" "$out"
    fi
}

# Copy those files from the input directory to the next generation that haven't
# been analyzed yet.
function maybe_import() {
    if [ -n "$(ls -A $in)" ]; then
        local f
        for f in $in/*; do
            if grep -q "$(basename $f)" $work_dir/analyzed_inputs; then
                continue
            fi

            if [ -e "$work_dir/next/$(basename $f)" ]; then
                continue
            fi

            echo "Importing $f from the input directory"
            cp "$f" "$work_dir/next"
        done
    fi
}

# Set up the shell environment
export SYMCC_OUTPUT_DIR=$work_dir/symcc_out
export SYMCC_ENABLE_LINEARIZATION=1
# export SYMCC_AFL_COVERAGE_MAP=$work_dir/map

# Run generation after generation until we don't generate new inputs anymore
gen_count=0
gen_time_sum=0
touch solving_mark.txt
while true; do
    # Initialize the generation
    maybe_import
    mv $work_dir/{next,cur}
    mkdir $work_dir/next

    # Run it (or wait if there's nothing to run on)
    if [ -n "$(ls -A $work_dir/cur)" ]; then
        echo "Generation $gen_count..."
        echo "Generation $gen_count..." >> "$mark_file"
        gen_start_time=$(date +%s)
        echo "Generation $gen_count start time:$(date -d @$gen_start_time)" >> "$mark_file"

        for f in $work_dir/cur/*; do
            echo "Running on $f"
            echo "Running on $f" >> "$mark_file"

            if [[ "$target " =~ " @@ " ]]; then

                captured_output=$(env SYMCC_INPUT_FILE=$f $timeout ${target[@]/@@/$f} 2>&1)
                echo "$captured_output" >> "$mark_file"
                echo "$captured_output"
                processed_output=$(echo "$captured_output" | grep -E -o 'SMT: \{ "solving_time_once": ([0-9]+) \}')
                echo "$processed_output" >> "solving_mark.txt"
            else
                $timeout $target <$f >/dev/null 2>&1
            fi

            # Make the new test cases part of the next generation
            add_to_next_generation $work_dir/symcc_out
            maybe_export $work_dir/symcc_out
            echo $(basename $f) >> $work_dir/analyzed_inputs
            rm -f $f
        done

        rm -rf $work_dir/cur
        gen_count=$((gen_count+1))

        gen_end_time=$(date +%s)
        echo "Generation $gen_count end time:$(date -d @$gen_start_time)" >> "$mark_file"
        gen_run_time=$((gen_end_time - gen_start_time))
        gen_time_sum=$((gen_time_sum + gen_run_time))
        echo "Generation $gen_count run time is: $gen_run_time s"
        echo "Generation $gen_count run time is: $gen_run_time s" >> "$mark_file"


    else
        echo "Waiting for more input..."
        rmdir $work_dir/cur
        sleep 5
    fi

    # check file exitance
    if [ -e "target" ]; then
        break
    fi

done

# Caculate solving_time_avg and gen_time_avg then site to the mark_file. 
solving_time_sum=$(grep -o '"solving_time_once": [0-9]*' solving_mark.txt | awk -F ": " '{ sum += $2 } END { print sum }')
solving_count=$(grep -E -c 'SMT: \{ "solving_time_once": ([0-9]+) \}' solving_mark.txt)
echo "The solving time sum is: $solving_time_sum s. The solving times is: $solving_count."
echo "The solving time sum is: $solving_time_sum s. The solving times is: $solving_count." >> "$mark_file"
solving_time_avg=$((solving_time_sum / solving_count))
echo "The average of solving time is: $solving_time_avg s."
echo "The average of solving time is: $solving_time_avg s." >> "$mark_file"
rm -rf solving_mark.txt

echo "The generation time sum is: $gen_time_sum s. The generation times is: $gen_count."
echo "The generation time sum is: $gen_time_sum s. The generation times is: $gen_count." >> "$mark_file"
gen_count=$((gen_count+1))
gen_time_avg=$((gen_time_sum / gen_count))
echo "The average of generation time is: $gen_time_avg s."
echo "The average of generation time is: $gen_time_avg s." >> "$mark_file"

# Get the end time
end_time=$(stat -c %Y "target")

# Caculate the total run time
total_run_time=$((end_time - start_time))

echo "The total run time is: $total_run_time s"

# Site to mark file
echo "END TIME:$(date -d @$end_time)" >> "$mark_file"
echo "The total run time is: $total_run_time s" >> "$mark_file"

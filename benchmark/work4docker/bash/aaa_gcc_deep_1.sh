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

# Remove out_s files
rm -rf "out_s/"*

# Get the start time
start_time=$(date +%s%3N)

# Declare the mark file
if [ -e "mark_deep.txt" ]; then
    rm -rf mark_deep.txt
fi
touch mark_deep.txt
mark_file="mark_deep.txt"
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

# Copy all files in the source directory to the destination directory, renaming
# them according to their hash.
function move_with_unique_name() {
    local source_dir="$1"
    local dest_dir="$2"

    if [ -n "$(ls -A $source_dir)" ]; then
        local f
        for f in $source_dir/*; do
            local dest="$dest_dir/$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | head -c 8)"
            mv "$f" "$dest"
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
gen_count=-1
dept_count=0
run_count=0
run_time_sum=0
wait_input_sum=0
input_stack=()

rm -rf ./inputs

mkdir ./inputs

if [ -e "solving_mark.txt" ]; then
    rm -rf solving_mark.txt
fi
touch solving_mark.txt
while true; do

    # check file existence
    if [ -e "target" ]; then
        break
    fi

    # Initialize the generation
    maybe_import
    mv $work_dir/{next,cur}
    mkdir $work_dir/next
    mkdir -p $work_dir/topush

    # Run it (or wait if there's nothing to run on)
    if [ -n "$(ls -A $work_dir/cur)" ]; then
        

        if [ ${#input_stack[@]} -eq 0 ]; then
            file_to_push="$(ls -A "$work_dir/cur" | head -n 1)"
            mkdir $work_dir/only
            cp $work_dir/cur/$file_to_push $work_dir/only/
            copy_with_unique_name "$work_dir/only" "./inputs"
            rm -rf $work_dir/only

            input_stack+=("$work_dir/cur/$file_to_push")
            dept_count=0
            gen_count=$((gen_count+1))
        fi

        while [ ${#input_stack[@]} -gt 0 ]; do
            f="${input_stack[-1]}"  # Peek at the top input
            

            echo "Generation $gen_count...Dept $dept_count..."
            echo "Generation $gen_count...Dept $dept_count..." >> "$mark_file"
            echo "Running on $f"
            echo "Running on $f" >> "$mark_file"
            
            run_start=$(date +%s%N)
            if [[ "$target " =~ " @@ " ]]; then

                # echo "env SYMCC_INPUT_FILE=$f $timeout ${target[@]/@@/@$f} 2>&1"
                captured_output=""
                if [[ "$target " =~ "cxxfilt" ]]; then
                    echo "env SYMCC_INPUT_FILE=$f $timeout ${target[@]/@@/@$f} 2>&1"
                    captured_output=$(env SYMCC_INPUT_FILE=$f $timeout ${target[@]/@@/@$f} 2>&1)
                else
                    captured_output=$(env SYMCC_INPUT_FILE=$f $timeout ${target[@]/@@/$f} 2>&1)
                fi
                # captured_output=$(env SYMCC_INPUT_FILE=$f $timeout ${target[@]/@@/$f} 2>&1)
                echo "$captured_output" >> "$mark_file"
                echo "$captured_output"
                processed_output=$(echo "$captured_output" | grep -E -o 'SMT: \{ "solving_time_once": ([0-9]+) \}')
                echo "$processed_output" >> "solving_mark.txt"
                
            else
                $timeout $target <$f >/dev/null 2>&1
            fi
            run_end=$(date +%s%N)
            run_count=$((run_count+1))
            run_time=$((run_end - run_start))
            echo "run time is $((run_time / 1000000)) ms"
            echo "run time is $((run_time / 1000000)) ms" >> "$mark_file"
            run_time_sum=$((run_time_sum + run_time / 1000000))

            echo $(basename $f) >> $work_dir/analyzed_inputs

            # Pop input from the stack
            input_stack=("${input_stack[@]:0:${#input_stack[@]}-1}")
            
            maybe_export $work_dir/symcc_out
            if [ -n "$(ls -A $work_dir/symcc_out/)" ];then
                echo $(ls $work_dir/symcc_out/)
                move_with_unique_name "$work_dir/symcc_out" "$work_dir/topush"

                copy_with_unique_name "$work_dir/symcc_out" "./inputs"
                for file_path in "$work_dir/topush/"*; do
                    if grep -q "$(basename $file_path)" $work_dir/analyzed_inputs; then
                        continue
                    fi
                    if [ -f "$file_path" ]; then
                        # Push input onto the stack
                        input_stack+=("$file_path")
                        echo "add $file_path into input_stack"
                        echo "add $file_path into input_stack" >> "$mark_file"
                        dept_count=$((dept_count+1))
                    fi
                done
                
            fi

            # check file existence
            if [ -e "target" ]; then
                echo "The target_file was produced by file: $f"
                dest_1="out_s"
                cp "$f" "$dest_1"
                break
            fi

            rm -f $f

            

        done
        
        # copy_with_unique_name "$work_dir/topush" "./inputs"
        rm -rf $work_dir/cur
        rm -rf $work_dir/topush
        
        
    else
        if [ $wait_input_sum -gt 10 ]; then
            break
        fi
        echo "Waiting for more input..."
        maybe_import
        rmdir $work_dir/cur
        rm -rf $work_dir/topush
        wait_input_sum=$((wait_input_sum+1))
        sleep 5
    fi
done

solving_time_sum=$(grep -o '"solving_time_once": [0-9]*' solving_mark.txt | awk -F ": " '{ sum += $2 } END { print sum }')
solving_count=$(grep -E -c 'SMT: \{ "solving_time_once": ([0-9]+) \}' solving_mark.txt)
echo "The solving time sum is: $solving_time_sum us. The solving times is: $solving_count."
echo "The solving time sum is: $solving_time_sum us. The solving times is: $solving_count." >> "$mark_file"
if [ $solving_count -gt 0 ]; then
    solving_time_avg=$((solving_time_sum / solving_count))
    echo "The average of solving time is: $solving_time_avg us."
    echo "The average of solving time is: $solving_time_avg us." >> "$mark_file"
fi
rm -rf solving_mark.txt

echo "The run time sum is: $run_time_sum ms. The run times is: $run_count."
echo "The run time sum is: $run_time_sum ms. The run times is: $run_count." >> "$mark_file"
run_time_avg=$((run_time_sum / run_count))
echo "The average of run time is: $run_time_avg ms."
echo "The average of run time is: $run_time_avg ms." >> "$mark_file"




# Get the end time
end_time=$(date +%s%3N -r "target")

# Caculate the total run time
total_run_time=$((end_time - start_time))

echo "The total run time is: $total_run_time ms"

# Site to mark file
echo "END TIME:$(date -d @$end_time)" >> "$mark_file"
echo "The total run time is: $total_run_time ms" >> "$mark_file"

#! /bin/sh

includes () {
  cat project.json | \
    jq -r '.["m5stack-atom"]["includes"]["build", "toolchain"] | join(" ")'
}

flags () {
  cat project.json | \
    jq -r '.["m5stack-atom"]["cc_flags", "cxx_flags"] | join(" ")'
}

rm -f compile_flags.txt
pipenv run pio project metadata --json-output > project.json

for DIR in $(includes)
do
  echo "-I${DIR}" >> compile_flags.txt
done

echo "-DESP32" >> compile_flags.txt
echo "-DARDUINO=1" >> compile_flags.txt

# for FLAG in $(flags)
# do
#   echo "${FLAG}" >> compile_flags.txt
# done

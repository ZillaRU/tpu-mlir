#!/bin/bash
set -ex

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# run onnx operation test
test_onnx.py

# run models
model_list_basic=(
  "yolov5s"
  "resnet18"
  "resnet50_v2"
  "mobilenet_v2"
  "squeezenet1.0"
)

for net in ${model_list_basic[@]}
do
  echo "======= test $net ====="
  $DIR/run_model.sh $net
  echo "test $net success"
done

echo "test basic models success"

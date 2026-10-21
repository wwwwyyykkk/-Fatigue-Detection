import math, random, requests, sys
from pathlib import Path
import tensorflow as tf
from tensorflow.keras import Model
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import (
    Dense, InputLayer, Dropout, Conv1D, Flatten, Reshape, MaxPooling1D, BatchNormalization,
    Conv2D, GlobalMaxPooling2D, Lambda, GlobalAveragePooling2D, Rescaling)
from tensorflow.keras.optimizers.legacy import Adam, Adadelta
from tensorflow.keras.losses import categorical_crossentropy

sys.path.append('./resources/libraries')
import ei_tensorflow.training

WEIGHTS_PATH = './transfer-learning-weights/edgeimpulse/MobileNetV2.0_35.96x96.grayscale.bsize_64.lr_0_005.epoch_260.val_loss_3.10.val_accuracy_0.35.hdf5'

# Download the model weights
root_url = 'https://cdn.edgeimpulse.com/'
p = Path(WEIGHTS_PATH)
if not p.exists():
    print(f"Pretrained weights {WEIGHTS_PATH} unavailable; downloading...")
    if not p.parent.exists():
        p.parent.mkdir(parents=True)
    weights_data = requests.get(root_url + WEIGHTS_PATH[2:]).content
    with open(WEIGHTS_PATH, 'wb') as f:
        f.write(weights_data)
    print(f"Pretrained weights {WEIGHTS_PATH} unavailable; downloading OK")
    print("")

INPUT_SHAPE = (96, 96, 1)

base_model = tf.keras.applications.MobileNetV2(
    input_shape = INPUT_SHAPE, alpha=0.35,
    weights = WEIGHTS_PATH
)

base_model.trainable = False

model = Sequential()
model.add(InputLayer(input_shape=INPUT_SHAPE, name='x_input'))
# ── 归一化层：[0,1] → [-1,1]（MobileNetV2 预训练权重要求的输入范围）
# Edge Impulse 内部已将图像归一化到 [0,1]，所以 scale=2.0 即可
# 这一层会被 bake 进导出的 TFLite，板子上送 [0,1]（gray96[i]/255.0f）
model.add(Rescaling(scale=2.0, offset=-1))
# Don't include the base model's top layers
last_layer_index = -3
model.add(Model(inputs=base_model.inputs, outputs=base_model.layers[last_layer_index].output))
model.add(Reshape((-1, model.layers[-1].output.shape[3])))
model.add(Dense(16, activation='relu'))
model.add(Dropout(0.25))  # drop=0.25 for MobileNetV2 0.35
model.add(Flatten())
model.add(Dense(classes, activation='softmax'))


# ═══════════════════════════════════════════════════════════
# 数据增强 - 使用纯 tf.image API（保证在 Edge Impulse 环境可用）
# ═══════════════════════════════════════════════════════════
def augment_image(image, label):
    # 1. 水平翻转
    image = tf.image.random_flip_left_right(image)

    # 2. 缩放 + 随机裁剪（模拟远近变化）
    rand_scale = tf.random.uniform((), 1.0, 1.25)
    new_size = tf.cast(tf.math.floor(rand_scale * 96), tf.int32)
    image = tf.image.resize_with_crop_or_pad(image,
                                              tf.cast(new_size, tf.int32),
                                              tf.cast(new_size, tf.int32))
    image = tf.image.random_crop(image, size=(96, 96, 1))

    # 3. 亮度 ±30%
    image = tf.image.random_brightness(image, max_delta=0.3)

    # 4. 对比度 ±30%（关键！让模型关注形状而非亮度）
    image = tf.image.random_contrast(image, lower=0.7, upper=1.3)

    # 5. 裁剪像素值到合法范围
    image = tf.clip_by_value(image, 0, 255)

    return image, label

train_dataset = train_dataset.map(augment_image, num_parallel_calls=tf.data.AUTOTUNE)

BATCH_SIZE = args.batch_size or 32
EPOCHS = args.epochs or 200   # 从 80 -> 120 轮，增强更多数据需要更多训练
LEARNING_RATE = args.learning_rate or 0.0005
ENSURE_DETERMINISM = args.ensure_determinism
if not ENSURE_DETERMINISM:
    train_dataset = train_dataset.shuffle(buffer_size=BATCH_SIZE*4)
prefetch_policy = 1 if ENSURE_DETERMINISM else tf.data.AUTOTUNE
train_dataset = train_dataset.batch(BATCH_SIZE, drop_remainder=False).prefetch(prefetch_policy)
validation_dataset = validation_dataset.batch(BATCH_SIZE, drop_remainder=False).prefetch(prefetch_policy)
callbacks.append(BatchLoggerCallback(BATCH_SIZE, train_sample_count, epochs=EPOCHS, ensure_determinism=ENSURE_DETERMINISM))


model.compile(optimizer=tf.keras.optimizers.Adam(learning_rate=LEARNING_RATE),
                loss='categorical_crossentropy',
                metrics=['accuracy'])
model.fit(train_dataset, validation_data=validation_dataset, epochs=EPOCHS, verbose=2, callbacks=callbacks)

print('')
print('Initial training done.', flush=True)

# Fine tuning
FINE_TUNE_EPOCHS = 30    # 从 10 -> 20
FINE_TUNE_PERCENTAGE = 70  # 从 65 -> 70（解冻更多层）

print('Fine-tuning best model for {} epochs...'.format(FINE_TUNE_EPOCHS), flush=True)

# Load best model from initial training
model = ei_tensorflow.training.load_best_model(BEST_MODEL_PATH)

# Determine which layer to begin fine tuning at
model_layer_count = len(model.layers)
fine_tune_from = math.ceil(model_layer_count * ((100 - FINE_TUNE_PERCENTAGE) / 100))

# Allow the entire base model to be trained
model.trainable = True
# Freeze all the layers before the 'fine_tune_from' layer
for layer in model.layers[:fine_tune_from]:
    layer.trainable = False

model.compile(optimizer=tf.keras.optimizers.Adam(learning_rate=0.000045),
                loss='categorical_crossentropy',
                metrics=['accuracy'])

model.fit(train_dataset,
                epochs=FINE_TUNE_EPOCHS,
                verbose=2,
                validation_data=validation_dataset,
                callbacks=callbacks,
                class_weight=None
            )

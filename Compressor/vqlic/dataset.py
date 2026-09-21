"""Image-folder dataset and the base_v3 transform pipeline.

Data lives in [-1, 1] (`Normalize(0.5, 0.5)` after `ToTensor`), which matches the
decoder's final Tanh and the `data_range=2.0` passed to SSIM in the loss. Changing
one of those three without the others silently rescales the distortion term.

A source is either a directory of images or a FiftyOne Zoo split (see `ZooSpec`
and `resolve_source`). Both end up as a list of paths handed to the same
`ImageFolderDataset` -- FiftyOne is used as a downloader, never as a Dataset.
"""
from __future__ import annotations

import os
from typing import NamedTuple

import numpy as np
import torch
from PIL import Image
from torch.utils.data import DataLoader, Dataset, Sampler
from torchvision.transforms import (
    CenterCrop, Compose, Normalize, RandomChoice, RandomCrop,
    RandomHorizontalFlip, RandomResizedCrop, Resize, ToTensor,
)

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tif", ".tiff")


class ZooSpec(NamedTuple):
    """Which FiftyOne Zoo split to pull, when no directory was given."""
    name: str = "open-images-v7"
    split: str = "validation"
    max_samples: int = 0           # 0 = the whole split (this can be enormous)
    shuffle: bool = True           # picks WHICH samples, not the training order
    seed: int = 0
    dataset_dir: str = ""          # override fiftyone's zoo root


def load_zoo_files(spec, verbose=True):
    """Download (or reuse) a Zoo split and return its image paths, sorted.

    FiftyOne is a downloader here and nothing else. It is deliberately NOT used
    as a torch Dataset: a `fo.Dataset` is a view onto a local MongoDB, and a
    handle to it does not survive the fork that spawns DataLoader workers -- each
    worker would have to re-open its own pymongo connection per sample. Reading
    the filepaths out once, in the parent, gives a plain list and keeps every
    property of the folder path (listing cache, resume sampler, corrupt-file
    tolerance) intact.

    The import is local for the same reason it is worth avoiding: `import
    fiftyone` starts a `mongod` process. Nothing that only trains from a folder
    should pay for that.

    `label_types=[]` matters. This codec never reads an annotation -- base_v7's
    default label download is detections + classifications + segmentations plus
    the class metadata CSVs, which is gigabytes of work for images you will crop
    to 224 and never label.
    """
    try:
        import fiftyone as fo
        import fiftyone.zoo as foz
    except ImportError as e:
        raise RuntimeError(
            "a zoo source needs FiftyOne:  pip install fiftyone\n"
            "  (or pass --train-dir and skip it entirely)") from e

    kw = {
        "split": spec.split,
        "shuffle": bool(spec.shuffle),
        "seed": int(spec.seed),
        # An explicit, parameter-derived name. FiftyOne's default is
        # "<name>-<split>", so a later run asking for more images silently reuses
        # the smaller cached dataset; encoding max_samples keeps them distinct.
        "dataset_name": f"nic-{spec.name}-{spec.split}-{spec.max_samples or 'all'}",
        "persistent": False,       # the DB entry is disposable; the JPEGs on disk are not
    }
    if spec.max_samples:
        kw["max_samples"] = int(spec.max_samples)
    if spec.dataset_dir:
        kw["dataset_dir"] = spec.dataset_dir

    if verbose:
        print(f"FiftyOne Zoo: loading {spec.name}/{spec.split}"
              f"{f' (max {spec.max_samples:,})' if spec.max_samples else ' (FULL SPLIT)'}"
              f" seed={spec.seed} ...")
    try:
        ds = foz.load_zoo_dataset(spec.name, label_types=[], **kw)
    except (TypeError, ValueError):
        # Not every zoo downloader takes label_types; the ones that do not have
        # no labels to skip anyway.
        ds = foz.load_zoo_dataset(spec.name, **kw)

    paths = sorted(ds.values("filepath"))
    if not paths:
        raise RuntimeError(f"zoo split {spec.name}/{spec.split} returned no images")
    if verbose:
        common = os.path.commonpath(paths) if len(paths) > 1 else paths[0]
        if not os.path.isdir(common):
            common = os.path.dirname(common)
        print(f"  {len(paths):,} images under {common}")
        print(f"  these are full-size JPEGs -- consider "
              f"`python scripts/prepare_images.py {common} <dst>` once, then "
              f"--train-dir <dst>, so decoding stops being the bottleneck")
    return paths


def resolve_source(root_dir, zoo=None, file_list="", verbose=True):
    """(root_dir, names) for either a directory or a zoo spec.

    A directory always wins; the zoo is the fallback for "no data_path given".
    An existing `file_list` short-circuits the zoo entirely, which is the point
    of caching it: the exact subset chosen on run 1 is what run 2 resumes on,
    and a restart never re-imports FiftyOne. `names` of None means
    `ImageFolderDataset` sources them itself (cache, else listdir).
    """
    if root_dir:
        return root_dir, None
    if zoo is None:
        raise ValueError(
            "no data source: pass a directory, or a ZooSpec to download one")
    if file_list and os.path.exists(file_list):
        if verbose:
            print(f"zoo subset pinned by {file_list}; not calling FiftyOne")
        return "", None
    # Absolute paths with an empty root, so the cached list stays valid no matter
    # where fiftyone put the images.
    return "", load_zoo_files(zoo, verbose=verbose)


class ImageFolderDataset(Dataset):
    """Every image directly under `root_dir`, sorted by filename.

    base_v3 called this OpenImagesV7Dataset, but it never used any annotations --
    it is a flat image folder reader, so it is named for what it does. `sorted()`
    is kept because a stable order is what makes `shuffle=False` reproducible.

    `file_list` caches the listing. OpenImages train is 1.74M files in one flat
    directory, where `os.listdir` + `sorted` costs tens of seconds and a few
    hundred MB every single start -- and a preemptible cloud instance starts a
    lot. `limit` truncates the (sorted, hence stable) list, for smoke runs.
    """

    def __init__(self, root_dir, transform=None, verbose=True, file_list="",
                 limit=0):
        self.root_dir = root_dir
        self.transform = transform
        if not os.path.isdir(root_dir):
            raise FileNotFoundError(f"dataset directory does not exist: {root_dir}")

        names = None
        if file_list and os.path.exists(file_list):
            with open(file_list, "r", encoding="utf-8") as f:
                names = [ln.strip() for ln in f if ln.strip()]
            if verbose:
                print(f"Read {len(names):,} filenames from cache {file_list}")
        if names is None:
            names = sorted(f for f in os.listdir(root_dir)
                           if f.lower().endswith(IMAGE_EXTS))
            if file_list:
                os.makedirs(os.path.dirname(os.path.abspath(file_list)),
                            exist_ok=True)
                with open(file_list, "w", encoding="utf-8") as f:
                    f.write("\n".join(names))
                if verbose:
                    print(f"Wrote listing cache {file_list} "
                          f"({len(names):,} names)")
        if not names:
            raise RuntimeError(f"no images found in {root_dir}")
        if limit:
            names = names[:int(limit)]

        # Held as a fixed-width bytes array rather than a list of str. At 1.74M
        # entries the list costs ~130 MB, and because CPython touches an object's
        # refcount just to read it, each fork'ed DataLoader worker gradually
        # copies all of it -- copy-on-write does not survive refcounting. The
        # bytes array is ~40 MB and is never refcounted per element. Non-ASCII
        # filenames cannot be encoded that way, so fall back to the plain list.
        try:
            self.image_files = np.array(names, dtype="S")
            self._encoded = True
        except (UnicodeEncodeError, SystemError):
            self.image_files = names
            self._encoded = False

        if verbose:
            print(f"Found {len(self.image_files):,} images in {root_dir}")

    def __len__(self):
        return len(self.image_files)

    def name(self, idx):
        n = self.image_files[idx]
        return n.decode() if self._encoded else n

    def __getitem__(self, idx):
        path = os.path.join(self.root_dir, self.name(idx))
        try:
            image = Image.open(path).convert("RGB")
            if self.transform:
                image = self.transform(image)
            return image
        except Exception as e:
            # Returning None and filtering in collate_fn keeps one corrupt file
            # from killing a multi-day run. The batch is short by one when it
            # happens, which is why drop_last is on for training.
            print(f"Error loading image {path}: {e}")
            return None


def collate_fn(batch):
    batch = [item for item in batch if item is not None]
    if not batch:
        return None
    return torch.utils.data.dataloader.default_collate(batch)


def train_transform(image_size, crop_only=False):
    """base_v3's training augmentation, or crop-only.

    base_v3 picked uniformly between `RandomCrop` and `RandomResizedCrop`, so half
    of all samples were RESAMPLED -- and `RandomResizedCrop`'s default scale range
    starts at 0.08, meaning it can take 8% of the image area and UPSAMPLE it to
    224. For a codec that is the worst kind of augmentation: it teaches the model
    that interpolated, high-frequency-free content is normal, and the model then
    spends its rate budget accordingly.

    `crop_only=True` keeps only the native-resolution random crop, so every pixel
    the codec ever sees came out of a JPEG at its original scale.
    """
    crop = RandomCrop(size=image_size, pad_if_needed=True, padding_mode="reflect")
    return Compose([
        crop if crop_only else RandomChoice([crop,
                                             RandomResizedCrop(size=image_size)]),
        RandomHorizontalFlip(),
        ToTensor(),
        Normalize((0.5, 0.5, 0.5), (0.5, 0.5, 0.5)),   # -> [-1, 1]
    ])


def eval_transform(image_size, resize_short=None):
    """Deterministic eval transform.

    base_v3's `_eval_transform` referenced a bare `transforms.Resize`, but never
    imported `transforms`, so calling it raised NameError -- it was dead code. The
    working version base_v3 actually used at eval time (`build_clic_loader`) was a
    plain CenterCrop with the Resize commented out, so that is the default here.
    Pass `resize_short` to resize the short side first.
    """
    steps = []
    if resize_short:
        steps.append(Resize(resize_short))
    steps += [
        CenterCrop((image_size, image_size)),
        ToTensor(),
        Normalize((0.5, 0.5, 0.5), (0.5, 0.5, 0.5)),
    ]
    return Compose(steps)


class RotatingSequentialSampler(Sampler):
    """Sequential order that starts at `start` and wraps once.

    This is what makes a resumed sequential run correct. With `shuffle=False` the
    order is deterministic, so a loader that always restarts at index 0 re-trains
    on the same leading images after every restart and never reaches the tail of
    the corpus -- harmless under shuffling, silently corpus-limiting without it.

    Generator-based rather than a materialized index list: at 1.74M images a list
    of Python ints costs ~14 MB per worker for no reason.
    """

    def __init__(self, n, start=0):
        self.n = int(n)
        self.start = int(start) % max(self.n, 1)

    def __iter__(self):
        n, s = self.n, self.start
        return iter((s + i) % n for i in range(n))

    def __len__(self):
        return self.n


def build_trainloader(root_dir, batch_size=16, image_size=224, num_workers=0,
                      shuffle=False, seed=None, start_index=0, verbose=True,
                      scales=(), file_list="", limit=0, crop_only=False):
    """Training loader.

    `shuffle=False` reproduces base_v3. Pass `start_index` (in IMAGES, not
    batches) to resume a sequential run where it left off -- see
    `RotatingSequentialSampler`.

    `scales` enables multi-resolution training. The crop size becomes max(scales)
    and the ENGINE downsamples each batch to a randomly chosen scale, rather than
    the transform varying per sample. Two reasons it works this way:

    * a batch has to be one shape to collate at all, so the size must be chosen
      per batch, not per sample -- and a shared mutable size on the dataset breaks
      the moment num_workers > 0, because each worker gets its own copy;
    * cropping at the largest size means every smaller scale is produced by
      DOWNsampling. Cropping at 224 and upsampling to 352 would instead teach the
      codec that high resolution means blurry.
    """
    if scales:
        image_size = max(scales)
    dataset = ImageFolderDataset(
        root_dir, transform=train_transform(image_size, crop_only=crop_only),
        verbose=verbose, file_list=file_list, limit=limit)
    generator = None
    sampler = None
    if shuffle:
        if seed is not None:
            generator = torch.Generator().manual_seed(int(seed))
    elif start_index:
        sampler = RotatingSequentialSampler(len(dataset), start_index)
        if verbose:
            print(f"  sequential resume: starting at image {sampler.start:,} "
                  f"of {len(dataset):,}")
    return DataLoader(
        dataset, batch_size=batch_size,
        shuffle=shuffle if sampler is None else False,
        sampler=sampler, drop_last=True,
        num_workers=num_workers, pin_memory=True, collate_fn=collate_fn,
        generator=generator,
        persistent_workers=num_workers > 0,
        prefetch_factor=4 if num_workers > 0 else None,
    )


def build_valloader(root_dir, batch_size=16, image_size=224, num_workers=0,
                    resize_short=None, verbose=True):
    dataset = ImageFolderDataset(
        root_dir, transform=eval_transform(image_size, resize_short), verbose=verbose)
    return DataLoader(
        dataset, batch_size=batch_size, shuffle=False, drop_last=False,
        num_workers=num_workers, pin_memory=True, collate_fn=collate_fn,
        persistent_workers=num_workers > 0,
    )

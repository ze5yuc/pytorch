"""
:mod:`torch.optim._multi_tensor` is a package implementing various optimization algorithms.
Most commonly used methods are already supported, and the interface is general
enough, so that more sophisticated ones can be also easily integrated in the
future.
"""
from functools import partial
from torch import optim

from .adam import Adam
from .adamw import AdamW
NAdam = partial(optim.NAdam, foreach=True)
from .sgd import SGD
RAdam = partial(optim.RAdam, foreach=True)
from .rmsprop import RMSprop
from .rprop import Rprop
ASGD = partial(optim.ASGD, foreach=True)
Adamax = partial(optim.Adamax, foreach=True)
Adadelta = partial(optim.Adadelta, foreach=True)
Adagrad = partial(optim.Adagrad, foreach=True)

del adam
del adamw
del sgd
del rmsprop
del rprop

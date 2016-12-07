
template <class E, MEMFLAGS F, unsigned int N>
bool BufferedOverflowTaskQueue<E, F, N>::pop_buffer(E &t)
{
  if (_buf_empty) return false;
  t = _elem;
  _buf_empty = true;
  return true;
}

template <class E, MEMFLAGS F, unsigned int N>
inline bool BufferedOverflowTaskQueue<E, F, N>::push(E t)
{
  if (_buf_empty) {
    _elem = t;
    _buf_empty = false;
    return true;
  } else {
    return taskqueue_t::push(t);
  }
}

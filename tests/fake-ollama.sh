#!/bin/sh
# Stand-in for the `ollama` CLI used by ollamajobs-selftest (T-1).
# Offline, no daemon: every answer is canned. Model names pick the behaviour:
#   pull slow|fast|fail, create crash|slowc|<any>, rm fail-rm|<any>,
#   cp fail-cp|<any>, show <name> [--modelfile], list.
# Every call is appended to $FAKE_OLLAMA_LOG (when set), one line per call.
[ -n "$FAKE_OLLAMA_LOG" ] && echo "$*" >> "$FAKE_OLLAMA_LOG"
cmd=$1
shift
case "$cmd" in
pull)
    case "$1" in
    slow)
        i=1
        while [ $i -le 10 ]; do
            echo "pulling aaaa: $((i * 10))% ▕██ ▏ $((i * 100)) MB/1.0 GB"
            sleep 0.1
            i=$((i + 1))
        done ;;
    fail)
        echo "pulling manifest"
        echo "Error: pull model manifest: file does not exist"
        exit 1 ;;
    esac
    echo "pulling bbbb: 100% ▕████▏ 10 MB/10 MB"
    printf 'success'
    exit 0 ;;
create)
    [ "$1" = crash ] && kill -SEGV $$
    [ "$1" = slowc ] && sleep 0.6
    # The temporary Modelfile must be private (S-1): report its mode.
    [ -n "$FAKE_OLLAMA_LOG" ] && echo "modelfile-mode $(stat -c %a "$3") $3" >> "$FAKE_OLLAMA_LOG"
    echo success
    exit 0 ;;
rm)
    [ "$1" = fail-rm ] && { echo "Error: model is in use" >&2; exit 1; }
    echo "deleted '$1'"
    exit 0 ;;
cp)
    [ "$1" = fail-cp ] && { echo "Error: no such model" >&2; exit 1; }
    echo "copied '$1' to '$2'"
    exit 0 ;;
show)
    if [ "$2" = --modelfile ]; then
        case "$1" in
        a:1|b:1) blob=sha256-aaaa ;;
        *) blob=sha256-cccc ;;
        esac
        printf 'FROM /srv/ollama/blobs/%s\nSYSTEM """\nLine one\n\n    indented\n"""\nPARAMETER stop "<a>"\nPARAMETER stop "<b>"\n' "$blob"
    else
        printf '  Model\n    architecture        qwen3\n    parameters          8B\n    context length      40960\n\n  Parameters\n    stop    "<a>"\n    stop    "<b>"\n\n  System\n    Line one\n'
    fi
    exit 0 ;;
list)
    sleep 0.2
    printf 'NAME    ID              SIZE      MODIFIED\na:1     aaaa11112222    1 GB      2 days ago\nb:1     bbbb11112222    1 GB      2 days ago\nc:1     cccc11112222    2 GB      3 days ago\n'
    exit 0 ;;
esac
echo "unknown command $cmd" >&2
exit 1

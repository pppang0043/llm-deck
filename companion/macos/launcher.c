// LLM Deck.app 의 실행 파일. macOS 블루투스 권한을 이 앱 이름으로 받기 위해,
// 파이썬 도우미를 자식 프로세스로 띄우고 끝날 때까지 기다린다.
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static pid_t child = 0;
// 앱이 종료 신호를 받으면 도우미도 같이 끝낸다 (USB 포트를 붙잡은 채 남지 않도록)
static void forward(int sig) {
  if (child > 0) kill(child, sig);
}
int main(int argc, char **argv_in) {
  const char *dir = COMPANION_DIR;  // install.sh 가 컴파일할 때 넣어 준다
  if (chdir(dir) != 0) { perror("chdir"); return 1; }
  setenv("PYTHONUNBUFFERED", "1", 1);
  setenv("PATH", "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin", 1);
  // 받은 인자는 도우미에게 그대로 넘긴다 (예: --link ble)
  char *argv[32] = {".venv/bin/python", "main.py"};
  int n = 2;
  for (int i = 1; i < argc && n < 31; i++) argv[n++] = argv_in[i];
  argv[n] = NULL;
  signal(SIGTERM, forward);
  signal(SIGINT, forward);
  signal(SIGHUP, forward);
  if (posix_spawn(&child, argv[0], NULL, NULL, argv, environ) != 0) { perror("spawn"); return 1; }
  int status;
  while (waitpid(child, &status, 0) < 0) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
